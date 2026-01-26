// Switch statement: multiple case blocks
int day_type(int day) {
    switch (day) {
        case 0:  // Sunday
        case 6:  // Saturday
            return 0;  // weekend
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
            return 1;  // weekday
        default:
            return -1; // invalid
    }
}
