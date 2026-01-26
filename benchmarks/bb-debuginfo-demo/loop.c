// Loop: multiple basic blocks (entry, loop header, loop body, exit)
int sum_to_n(int n) {
    int sum = 0;
    for (int i = 1; i <= n; i++) {
        sum += i;
    }
    return sum;
}
