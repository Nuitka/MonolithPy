#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Include our np_embed header
#include "Include/np_embed.h"

int main() {
    // Test with a regular file first
    FILE *test_file = fopen("test_input.txt", "w");
    if (test_file) {
        fprintf(test_file, "Line 1\nLine 2 with more text\nLine 3\n");
        fclose(test_file);
    }

    // Test getline with our implementation
    FILE *file = fopen("test_input.txt", "r");
    if (!file) {
        printf("Failed to open test file\n");
        return 1;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    int line_num = 1;

    printf("Testing getline implementation:\n");
    while ((read = getline(&line, &len, file)) != -1) {
        printf("Line %d (length %zd): %s", line_num++, read, line);
    }

    free(line);
    fclose(file);

    // Clean up
    unlink("test_input.txt");

    printf("Test completed successfully!\n");
    return 0;
}
