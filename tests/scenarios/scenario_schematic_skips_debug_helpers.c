void _timing_delay_cycles(unsigned n) {
    while (n-- > 0) {
        __asm__ volatile("" ::: "memory");
    }
}

void timing_gpio_start(void) {
    _timing_delay_cycles(10);
}

void timing_gpio_stop(void) {
    _timing_delay_cycles(20);
}

int main(void) {
    timing_gpio_start();
    timing_gpio_stop();
    return 0;
}
