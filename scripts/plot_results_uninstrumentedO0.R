#!/usr/bin/env Rscript
#
# Thin wrapper around plot_results.R that adds the -O0 uninstrumented build as a
# second execution-time baseline. It does NOT reimplement any plotting.
#
# How it works:
#   1. Loads plot_results.R into an isolated environment via sys.source() (so its
#      main() does not run on load), allowing the data/functions to be patched first.
#   2. Registers an extra "uninstrumentedO0" series (label "Uninstrumented-O0") in
#      ALGORITHMS and ALG_STYLE.
#   3. Overrides load_uninstrumented_data so that, ONLY for the execution_time_us
#      metric, it also loads uninstrumentedO0.csv alongside the usual
#      uninstrumented.csv. All other metrics are unchanged.
#   4. Runs main() from the patched environment.
#
# Net effect: identical to plot_results.R, except the execution-time chart shows
# both the optimized and the -O0 uninstrumented baselines side by side. Use this
# when comparing instrumented algorithms against the -O0 reference (apples-to-apples
# for execution time, since the instrumentation passes run on -O0 IR).
# Requires uninstrumentedO0.csv in the result dir (produced by run_benchmarks.py).

script_args <- commandArgs(trailingOnly = FALSE)
script_file_arg <- script_args[grepl("^--file=", script_args)]
script_path <- if (length(script_file_arg) > 0) {
  sub("^--file=", "", script_file_arg[1])
} else {
  NA_character_
}

plot_script_path <- if (!is.na(script_path)) {
  file.path(dirname(normalizePath(script_path)), "plot_results.R")
} else {
  file.path("scripts", "plot_results.R")
}

plot_env <- new.env(parent = globalenv())
sys.source(plot_script_path, envir = plot_env)

plot_env$ALGORITHMS <- c(plot_env$ALGORITHMS, "uninstrumentedO0")
plot_env$ALG_STYLE <- dplyr::bind_rows(
  plot_env$ALG_STYLE,
  tibble::tibble(
    algo = "uninstrumentedO0",
    label = "Uninstrumented-O0",
    color = "#9E9E9E",
    pattern = "stripe",
    pattern_angle = 135
  )
)

plot_env$load_uninstrumented_data <- function(result_dir, column) {
  load_single_uninstrumented <- function(filename, algo_name) {
    path <- file.path(result_dir, filename)
    if (!file.exists(path)) return(tibble::tibble())

    df <- plot_env$read_result_csv(path)
    if (nrow(df) == 0) return(tibble::tibble())

    df <- dplyr::filter(df, !is.na(benchmark), benchmark != "")

    if (!column %in% names(df)) return(tibble::tibble())

    tibble::tibble(
      benchmark = df$benchmark,
      cap = NA_character_,
      algo = algo_name,
      value = as.numeric(df[[column]])
    ) %>%
      dplyr::filter(!is.na(value))
  }

  base_uninstrumented <- load_single_uninstrumented(
    "uninstrumented.csv",
    "uninstrumented"
  )

  if (!identical(column, "execution_time_us")) {
    return(base_uninstrumented)
  }

  uninstrumented_o0 <- load_single_uninstrumented(
    "uninstrumentedO0.csv",
    "uninstrumentedO0"
  )

  dplyr::bind_rows(base_uninstrumented, uninstrumented_o0)
}

if (sys.nframe() == 0) {
  plot_env$main()
}
