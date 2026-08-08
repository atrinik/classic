#include <toolkit/bresenham.h>

#include <stddef.h>

static int check_line(int dx, int dy) {
    int x = 0;
    int y = 0;
    int fraction;
    int stepx;
    int stepy;
    int dx2;
    int dy2;
    size_t steps = 0;
    size_t expected_steps = (size_t) (dx < 0 ? -dx : dx);
    size_t y_steps = (size_t) (dy < 0 ? -dy : dy);

    if (y_steps > expected_steps) {
        expected_steps = y_steps;
    }

    BRESENHAM_INIT(dx, dy, fraction, stepx, stepy, dx2, dy2);

    while (x != dx || y != dy) {
        if (steps++ >= expected_steps) {
            return 1;
        }

        BRESENHAM_STEP(x, y, fraction, stepx, stepy, dx2, dy2);
    }

    return steps == expected_steps ? 0 : 1;
}

int main(void) {
    static const int endpoints[][2] = {
        {7, 2},
        {2, 7},
        {-2, 7},
        {-7, 2},
        {-7, -2},
        {-2, -7},
        {2, -7},
        {7, -2},
        {7, 0},
        {-7, 0},
        {0, 7},
        {0, -7},
        {0, 0},
    };

    for (size_t i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); i++) {
        if (check_line(endpoints[i][0], endpoints[i][1]) != 0) {
            return 1;
        }
    }

    return 0;
}
