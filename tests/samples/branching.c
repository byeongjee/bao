int abs_val(int x) {
    if (x < 0) {
        return -x;
    } else {
        return x;
    }
}

int main() {
    int result = abs_val(-5);
    return result;
}
