#pragma once
/* Number conversion reports range errors; there is no file access. */
int *__errno_location(void);
#define errno (*__errno_location())
#define ERANGE 34
