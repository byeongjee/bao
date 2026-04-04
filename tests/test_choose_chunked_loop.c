volatile int countdown_step = 1;

int countdown_sum(int n) {
    int sum = 0;
    while (n > 0) {
        sum += n;
        n -= countdown_step;
    }
    return sum;
}
