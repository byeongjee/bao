// Simple branching: 3 basic blocks (entry, if.then, if.else merged at return)
int simple_branch(int x) {
    if (x > 0) {
        return x * 2;
    }
    return x + 1;
}
