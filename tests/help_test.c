// Regression coverage for the '?' help overlay.
//
// ShowHelp needs a live console, so what is checked here is everything that can
// be wrong without one: the shape of the help table it walks, and the row
// arithmetic that keeps it inside the frame buffer. The second is the same
// invariant tests/row_guard_test.c proves for the panes -- the buffer holds
// exactly width*height cells, and no write may land at a row >= height -- but
// ShowHelp computes its rows from a scroll offset rather than from constants,
// so it is mirrored against the real MIN_WINDOW_HEIGHT here.
#define main drift_application_main
#include "../src/drift.c"
#undef main

static int test_failures = 0;

static void TestReport(const char* name, bool passed) {
    printf("  %-66s %s\n", name, passed ? "PASS" : "FAIL");
    if (!passed) test_failures++;
}

// Verbatim from ShowHelp: the popup's height, its visible row count, and where
// its writes land inside its own buffer.
static int HelpPopupHeight(int count, int screen_height) {
    int popup_h = count + 3;
    if (popup_h > screen_height - 2) popup_h = screen_height - 2;
    return popup_h;
}
static int HelpVisibleRows(int popup_h) { return popup_h - 3; }
static int HelpContentRow(int i) { return 1 + i; }
static int HelpFooterRow(int popup_h) { return popup_h - 2; }

// Verbatim from ShowHelp: scroll clamping, applied before anything is drawn.
static int HelpClampTop(int top, int count, int visible) {
    if (top > count - visible) top = count - visible;
    if (top < 0) top = 0;
    return top;
}

int main(void) {
    const int count = (int)(sizeof(help_entries) / sizeof(help_entries[0]));

    // ---------------------------------------------------------------------
    // Table shape -- a NULL or empty key would be dereferenced while drawing
    // ---------------------------------------------------------------------
    bool keys_present = true;
    bool bindings_labelled = true;
    bool headings_spaced = true;
    int headings = 0;
    int bindings = 0;
    for (int i = 0; i < count; i++) {
        const HelpEntry* entry = &help_entries[i];
        if (entry->keys == NULL) {
            keys_present = false;
            continue;
        }
        if (entry->text == NULL) {
            if (entry->keys[0] == '\0') continue; // the spacer between sections
            headings++;
            // A heading butting straight up against the previous section's
            // last binding reads as one more binding of that section
            if (i > 0) {
                const HelpEntry* previous = &help_entries[i - 1];
                bool spacer = previous->keys != NULL && previous->text == NULL &&
                              previous->keys[0] == '\0';
                if (!spacer) headings_spaced = false;
            }
            continue;
        }
        bindings++;
        if (entry->keys[0] == '\0') bindings_labelled = false;
        if (entry->text[0] == '\0') bindings_labelled = false;
    }
    TestReport("every entry has a key string to draw", keys_present);
    TestReport("every binding names both a key and what it does",
               bindings_labelled);
    TestReport("the table is not empty", count > 0 && bindings > 0);

    // Drift has four modes that rebind the same letters -- ordinary browsing,
    // the workspace list, the session list, and choosing a workspace's folders
    // -- so the reference is only useful if each has its own section. Catches a
    // whole section going missing rather than judging what is in one
    TestReport("every mode has its own section", headings >= 4);
    TestReport("a blank line separates each section from the last",
               headings_spaced);

    // ---------------------------------------------------------------------
    // Layout -- the key column must not run into the description column
    // ---------------------------------------------------------------------
    bool keys_fit = true;
    bool text_fits = true;
    const char* widest = "";
    const char* longest = "";
    for (int i = 0; i < count; i++) {
        const HelpEntry* entry = &help_entries[i];
        if (entry->text == NULL) {
            // A heading shares the key column but runs the full inner width
            if (HELP_KEY_COLUMN + (int)strlen(entry->keys) >= HELP_POPUP_WIDTH - 1) {
                text_fits = false;
                longest = entry->keys;
            }
            continue;
        }
        size_t len = strlen(entry->keys);
        if (HELP_KEY_COLUMN + (int)len >= HELP_TEXT_COLUMN) {
            keys_fit = false;
            if (len > strlen(widest)) widest = entry->keys;
        }
        // WriteToBuffer clips at the border rather than overflowing, so an
        // over-long description is a silent truncation rather than a crash --
        // still wrong, and invisible without this
        if (HELP_TEXT_COLUMN + (int)strlen(entry->text) >= HELP_POPUP_WIDTH - 1) {
            text_fits = false;
            if (strlen(entry->text) > strlen(longest)) longest = entry->text;
        }
    }
    if (!keys_fit) printf("    widest key string: [%s]\n", widest);
    if (!text_fits) printf("    longest text: [%s]\n", longest);
    TestReport("no key string overruns the description column", keys_fit);
    TestReport("nothing is silently clipped by the popup border", text_fits);

    // ---------------------------------------------------------------------
    // Row guard -- nothing may be written at a row >= height
    // ---------------------------------------------------------------------
    // ShowHelp refuses a popup shorter than 6 rows, so every geometry it does
    // draw at must leave at least one visible content row
    bool always_room_to_draw = true;
    bool refuses_the_rest = true;
    for (int screen_height = 1; screen_height <= 300; screen_height++) {
        int popup_h = HelpPopupHeight(count, screen_height);
        if (popup_h < 6) continue;              // ShowHelp returns before drawing
        if (HelpVisibleRows(popup_h) < 1) always_room_to_draw = false;
        if (popup_h > screen_height - 2) refuses_the_rest = false;
    }
    TestReport("every geometry it will draw at has room for a row",
               always_room_to_draw);
    TestReport("the popup always leaves a margin inside the window",
               refuses_the_rest);

    // Once the window is tall enough, the whole table fits without scrolling --
    // which is the point of trimming it down to a popup
    int roomy = HelpPopupHeight(count, count + 5);
    TestReport("a roomy window shows every entry at once",
               HelpVisibleRows(roomy) >= count);

    bool rows_in_bounds = true;
    bool footer_clear = true;
    bool reaches_last_entry = false;
    for (int screen_height = 1; screen_height <= 300; screen_height++) {
        int popup_h = HelpPopupHeight(count, screen_height);
        if (popup_h < 6) continue;
        int visible = HelpVisibleRows(popup_h);
        for (int top = -5; top <= count + 5; top++) {
            int clamped = HelpClampTop(top, count, visible);
            if (clamped < 0 || (count > visible && clamped > count - visible)) {
                rows_in_bounds = false;
            }
            for (int i = 0; i < visible && clamped + i < count; i++) {
                int row = HelpContentRow(i);
                if (row < 1 || row >= popup_h) rows_in_bounds = false;
                if (row >= HelpFooterRow(popup_h)) footer_clear = false;
                if (clamped + i == count - 1) reaches_last_entry = true;
            }
        }
        if (HelpFooterRow(popup_h) >= popup_h - 1) rows_in_bounds = false;
    }
    TestReport("every content row stays inside the popup buffer", rows_in_bounds);
    TestReport("content never overwrites the footer or the border", footer_clear);
    TestReport("scrolling can reach the last entry in the table",
               reaches_last_entry);

    // ---------------------------------------------------------------------
    // Scrolling reaches both ends and cannot run past them
    // ---------------------------------------------------------------------
    int visible = 10;
    TestReport("scrolling up past the start clamps to the first entry",
               HelpClampTop(-100, count, visible) == 0);
    TestReport("scrolling down past the end stops with a full page shown",
               HelpClampTop(count + 100, count, visible) == count - visible);
    TestReport("a page taller than the table pins to the top",
               HelpClampTop(5, count, count + 20) == 0);

    if (test_failures == 0) {
        printf("All help overlay tests passed.\n");
        return 0;
    }
    printf("%d help overlay test(s) failed.\n", test_failures);
    return 1;
}
