#include "common/EnergyValidatorPass.h"
#include "common/BaseContext.h"
#include "common/BlockUtils.h"
#include "milp/EnergyModel.h"
#include "rockclimb/RockClimbContext.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>

using namespace llvm;

// Defined in PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> MILPConfigOpt;
// Defined in RockClimbPass.cpp
extern cl::opt<std::string> RockClimbConfigOpt;

// Local CLI options
namespace {

static cl::opt<std::string> ValidateCheckpointFunctionOpt(
    "validate-checkpoint-function",
    cl::desc("Additional function name to treat as checkpoint"),
    cl::value_desc("name"),
    cl::init(""));

static cl::opt<double> ValidateEpsilonOpt(
    "validate-epsilon",
    cl::desc("Floating-point tolerance for energy checks"),
    cl::init(1e-4));

static cl::opt<bool> ValidateVerboseOpt(
    "validate-verbose",
    cl::desc("Print per-block remaining energy to stderr"),
    cl::init(false));

enum class ValidateMode { MILP, RockClimb };

static cl::opt<ValidateMode> ValidateModeOpt(
    "validate-mode",
    cl::desc("Checkpoint algorithm mode for energy validation"),
    cl::values(
        clEnumValN(ValidateMode::MILP, "milp", "MILP checkpoint insertion"),
        clEnumValN(ValidateMode::RockClimb, "rockclimb", "RockClimb (PFI) checkpoint insertion")),
    cl::init(ValidateMode::MILP));

} // anonymous namespace

namespace checkpoint {

/// Compute the effective capacity for the given mode.
/// MILP: MILPEnergyParams::capacity (= E_buf) from milp-config
/// RockClimb: RockClimbParams::calculateESafe() (= E_input - E_restore) from rockclimb-config
static double computeEffectiveCapacity(ValidateMode mode) {
    switch (mode) {
    case ValidateMode::RockClimb: {
        RockClimbParams rcParams;
        if (!parseRockClimbParams(RockClimbConfigOpt.getValue(), rcParams)) {
            llvm::errs() << "Error: Failed to parse RockClimb config for validation\n";
            return 0.0;
        }
        return rcParams.calculateESafe();
    }
    case ValidateMode::MILP: {
        auto milpParams = parseMILPEnergyParams(MILPConfigOpt.getValue());
        if (!milpParams) {
            llvm::errs() << "Error: Failed to parse MILP config for validation\n";
            return 0.0;
        }
        return milpParams->capacity;
    }
    }
    llvm_unreachable("unknown validate mode");
}

/// Build the set of known checkpoint runtime function names whose call costs
/// should be excluded from block energy (the runtime accounts for them).
static std::set<std::string> buildExcludedFunctions(
    const std::string &userCheckpointFn) {
    std::set<std::string> excluded = {
        // MILP runtime
        "__region_prologue",
        "__region_epilogue",
        "__checkpoint_store_reg",
        "__checkpoint_store_mem",
        "__restore_reg",
        "__restore_mem",
        // RockClimb runtime
        "__rockclimb_check",
        "__rockclimb_save_reg",
        // Generic checkpoint
        "checkpoint",
        "__checkpoint",
        // Energy validator runtime (should not count its own functions)
        "__energy_violation",
    };
    if (!userCheckpointFn.empty()) {
        excluded.insert(userCheckpointFn);
    }
    return excluded;
}

/// Build the set of known runtime/validator NVM globals to exclude from
/// NVM access penalty counting.
static std::set<std::string> buildExcludedNvmGlobals() {
    return {
        "__nvm_regs",
        "__nvm_region_id",
        "__nvm_pc",
        "__nvm_sp",
        // All __ev_* globals are validator-internal
    };
}

/// Check if a global variable is an excluded NVM global (runtime or validator).
static bool isExcludedNvmGlobal(const GlobalVariable *GV,
                                 const std::set<std::string> &excludedNames) {
    StringRef name = GV->getName();
    if (name.starts_with("__ev_"))
        return true;
    return excludedNames.count(name.str()) > 0;
}

/// Get or create a double global variable in the module.
static GlobalVariable *getOrCreateDoubleGlobal(Module &M, StringRef name,
                                                double initVal) {
    if (auto *existing = M.getGlobalVariable(name))
        return existing;

    auto *ty = Type::getDoubleTy(M.getContext());
    auto *init = ConstantFP::get(ty, initVal);
    auto *gv = new GlobalVariable(M, ty, false, GlobalValue::ExternalLinkage,
                                  init, name);
    return gv;
}

/// Count the number of calls to excluded checkpoint functions in a block.
static unsigned countExcludedCalls(const BasicBlock &BB,
                                    const std::set<std::string> &excluded) {
    unsigned count = 0;
    for (const Instruction &I : BB) {
        if (const auto *CI = dyn_cast<CallInst>(&I)) {
            if (const Function *callee = CI->getCalledFunction()) {
                if (excluded.count(callee->getName().str()))
                    count++;
            }
        }
    }
    return count;
}

/// Count load/store instructions accessing NVM globals in a block.
static unsigned countNvmAccesses(const BasicBlock &BB, const Module &M,
                                  const std::set<std::string> &excludedNvmNames) {
    unsigned count = 0;
    for (const Instruction &I : BB) {
        const GlobalVariable *GV = nullptr;
        if (const auto *LI = dyn_cast<LoadInst>(&I)) {
            GV = dyn_cast<GlobalVariable>(LI->getPointerOperand()->stripPointerCasts());
        } else if (const auto *SI = dyn_cast<StoreInst>(&I)) {
            GV = dyn_cast<GlobalVariable>(SI->getPointerOperand()->stripPointerCasts());
        }
        if (GV && GV->hasSection() && GV->getSection() == ".nvm") {
            if (!isExcludedNvmGlobal(GV, excludedNvmNames))
                count++;
        }
    }
    return count;
}

/// Check if a function is a user function we should scope (not intrinsic,
/// not runtime, not a declaration-only stub we inserted).
static bool isUserFunction(const Function *callee,
                            const std::set<std::string> &excluded) {
    if (!callee)
        return false;
    if (callee->isIntrinsic())
        return false;
    if (excluded.count(callee->getName().str()))
        return false;
    // Only scope calls to functions that have a body (defined in module)
    // For external declarations, we cannot instrument them, so skip scoping.
    if (callee->isDeclaration())
        return false;
    return true;
}

PreservedAnalyses EnergyValidatorPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
    std::string configPath = EnergyConfigOpt.getValue();
    Module &M = *F.getParent();
    LLVMContext &Ctx = M.getContext();

    // Step 1: Create base context (estimator + CFG)
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto ctxResult = createBaseContext(F, LI, configPath, "energy-validate pass");

    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            errs() << ctxResult.errorMessage;
        }
        return PreservedAnalyses::all();
    }

    auto &ctx = *ctxResult.context;

    // Step 2: Compute effective capacity based on mode
    ValidateMode mode = ValidateModeOpt;
    double effectiveCapacity = computeEffectiveCapacity(mode);

    // Step 3: Parse overhead parameters (only needed for MILP mode)
    MILPEnergyParams milpParams{0,0,0,0,0,0,0,0,0,0};
    if (mode == ValidateMode::MILP) {
        auto milpParamsOpt = parseMILPEnergyParams(MILPConfigOpt.getValue());
        if (!milpParamsOpt) {
            errs() << "Error: Failed to parse MILP config for validation\n";
            return PreservedAnalyses::all();
        }
        milpParams = *milpParamsOpt;
    }
    double callCost = 0.0;
    // Get call instruction cost from estimator (via a dummy block's analysis is
    // impractical, so we read instruction_costs.call from config directly)
    {
        std::ifstream file(configPath);
        if (file.is_open()) {
            nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
            if (!config.is_discarded() && config.contains("energy_parameters")) {
                const auto &ep = config["energy_parameters"];
                if (ep.contains("instruction_costs")) {
                    const auto &ic = ep["instruction_costs"];
                    if (ic.contains("call"))
                        callCost = ic["call"].get<double>();
                }
            }
        }
    }

    double nvmPenalty = milpParams.nvmAccessPenalty;
    double epsilon = ValidateEpsilonOpt;

    // Step 4: Emit parameter globals (once per module)
    GlobalVariable *gvRemaining = getOrCreateDoubleGlobal(M, "__ev_energy_remaining", effectiveCapacity);
    getOrCreateDoubleGlobal(M, "__ev_capacity", effectiveCapacity);
    getOrCreateDoubleGlobal(M, "__ev_E_pro", milpParams.E_pro);
    getOrCreateDoubleGlobal(M, "__ev_E_epi", milpParams.E_epi);
    getOrCreateDoubleGlobal(M, "__ev_reg_store", milpParams.regStoreEnergy);
    getOrCreateDoubleGlobal(M, "__ev_reg_restore", milpParams.regRestoreEnergy);
    getOrCreateDoubleGlobal(M, "__ev_mem_store_per_byte", milpParams.memStoreEnergyPerByte);
    getOrCreateDoubleGlobal(M, "__ev_mem_restore_per_byte", milpParams.memRestoreEnergyPerByte);
    getOrCreateDoubleGlobal(M, "__ev_nvm_access_penalty", nvmPenalty);
    getOrCreateDoubleGlobal(M, "__ev_epsilon", epsilon);

    // Get capacity global for save/restore around calls
    GlobalVariable *gvCapacity = M.getGlobalVariable("__ev_capacity");

    // Step 5: Build excluded function/global sets
    auto excludedFns = buildExcludedFunctions(ValidateCheckpointFunctionOpt);
    auto excludedNvmNames = buildExcludedNvmGlobals();

    // Step 6: Declare __energy_violation function
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *PtrTy = PointerType::get(Ctx, 0);
    Type *DoubleTy = Type::getDoubleTy(Ctx);
    FunctionCallee violationFn = M.getOrInsertFunction(
        "__energy_violation", VoidTy, PtrTy, PtrTy, DoubleTy, DoubleTy);

    // Declare verbose print function if needed
    FunctionCallee verboseFn = {nullptr, nullptr};
    if (ValidateVerboseOpt) {
        // void __ev_verbose_print(const char *func, const char *block, double remaining)
        verboseFn = M.getOrInsertFunction(
            "__ev_verbose_print", VoidTy, PtrTy, PtrTy, DoubleTy);
    }

    // Step 7: For each basic block, compute adjusted energy and insert fsub
    for (BasicBlock &BB : F) {
        // Get base energy from CFG analysis
        double baseEnergy = ctx.cfg->getBlockInfo(&BB).energyCost;

        // Subtract energy for excluded calls (runtime handles those)
        unsigned numExcludedCalls = countExcludedCalls(BB, excludedFns);
        double adjustedEnergy = baseEnergy - (numExcludedCalls * callCost);

        // MILP mode: add NVM access penalties
        if (mode == ValidateMode::MILP) {
            unsigned nvmAccesses = countNvmAccesses(BB, M, excludedNvmNames);
            adjustedEnergy += nvmAccesses * nvmPenalty;
        }

        // Clamp to non-negative (shouldn't happen, but be safe)
        if (adjustedEnergy < 0.0)
            adjustedEnergy = 0.0;

        // Skip blocks with zero energy
        if (adjustedEnergy == 0.0)
            continue;

        // Insert fsub at first non-PHI instruction
        BasicBlock::iterator insertPt = BB.getFirstNonPHIIt();
        IRBuilder<> builder(&*insertPt);

        Value *remaining = builder.CreateLoad(DoubleTy, gvRemaining, "ev.rem");
        Value *cost = ConstantFP::get(DoubleTy, adjustedEnergy);
        Value *newRemaining = builder.CreateFSub(remaining, cost, "ev.sub");
        builder.CreateStore(newRemaining, gvRemaining);

        // Verbose output: print remaining energy after subtraction
        if (ValidateVerboseOpt) {
            std::string blockName = getBlockName(BB, F);
            Value *funcName = builder.CreateGlobalString(F.getName(), "ev.fn");
            Value *blkName = builder.CreateGlobalString(blockName, "ev.bn");
            builder.CreateCall(verboseFn, {funcName, blkName, newRemaining});
        }
    }

    // Step 8: Insert save/restore of __ev_energy_remaining around user calls
    // We need to collect call sites first, then modify, to avoid iterator issues.
    SmallVector<CallInst *, 16> userCalls;
    for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
            if (auto *CI = dyn_cast<CallInst>(&I)) {
                Function *callee = CI->getCalledFunction();
                if (isUserFunction(callee, excludedFns)) {
                    userCalls.push_back(CI);
                }
            }
        }
    }

    // Create save slot in entry block if needed
    AllocaInst *saveSlot = nullptr;
    if (!userCalls.empty()) {
        IRBuilder<> allocaBuilder(&F.getEntryBlock(),
                                   F.getEntryBlock().begin());
        saveSlot = allocaBuilder.CreateAlloca(DoubleTy, nullptr, "ev.save.slot");
    }

    for (CallInst *CI : userCalls) {
        // Save before call
        IRBuilder<> preBuild(CI);
        Value *saved = preBuild.CreateLoad(DoubleTy, gvRemaining, "ev.saved");
        preBuild.CreateStore(saved, saveSlot);

        // Reset for callee
        Value *cap = preBuild.CreateLoad(DoubleTy, gvCapacity, "ev.cap");
        preBuild.CreateStore(cap, gvRemaining);

        // Restore after call
        BasicBlock::iterator afterCall(CI);
        ++afterCall;
        IRBuilder<> postBuild(&*afterCall);
        Value *restored = postBuild.CreateLoad(DoubleTy, saveSlot, "ev.restored");
        postBuild.CreateStore(restored, gvRemaining);
    }

    // Step 9: Insert exit assertion before each ret
    // Collect return blocks first to avoid modifying the BB list while iterating
    SmallVector<BasicBlock *, 4> retBlocks;
    for (BasicBlock &BB : F) {
        if (isa<ReturnInst>(BB.getTerminator()))
            retBlocks.push_back(&BB);
    }

    for (BasicBlock *BB : retBlocks) {
        auto *ret = cast<ReturnInst>(BB->getTerminator());
        std::string bbName = getBlockName(*BB, F);

        IRBuilder<> builder(ret);
        Value *remaining = builder.CreateLoad(DoubleTy, gvRemaining, "ev.exit.rem");
        Value *negEps = ConstantFP::get(DoubleTy, -epsilon);
        Value *violated = builder.CreateFCmpOLT(remaining, negEps, "ev.exit.cmp");

        // Create violation block and continue block
        BasicBlock *violBB = BasicBlock::Create(
            Ctx, "ev.violation", &F);
        BasicBlock *contBB = BasicBlock::Create(
            Ctx, "ev.exit.ok", &F);

        builder.CreateCondBr(violated, violBB, contBB);

        // Remove the original ret from BB - the cond branch replaced it
        ret->removeFromParent();

        // Violation block: call __energy_violation and unreachable
        IRBuilder<> violBuilder(violBB);
        Value *funcName = violBuilder.CreateGlobalString(F.getName(), "ev.fn.viol");
        Value *blkName = violBuilder.CreateGlobalString(bbName, "ev.bn.viol");
        Value *capVal = violBuilder.CreateLoad(DoubleTy, gvCapacity, "ev.cap.viol");
        violBuilder.CreateCall(violationFn, {funcName, blkName, remaining, capVal});
        violBuilder.CreateUnreachable();

        // Continue block: put the ret here
        ret->insertInto(contBB, contBB->end());
    }

    // Step 10: Strip ".nvm" section from globals.
    // The validator runs on the host (not MSP430), and ".nvm" is an ELF-only
    // section that causes errors on Mach-O. We've already used the section
    // info for NVM penalty analysis above.
    for (GlobalVariable &GV : M.globals()) {
        if (GV.hasSection() && GV.getSection() == ".nvm") {
            GV.setSection("");
        }
    }

    return PreservedAnalyses::none();
}

} // namespace checkpoint
