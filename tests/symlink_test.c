// Regression coverage for the symlink row: "name -> target", ls -l style.
//
// Three things can be wrong here without a console or a filesystem to hand.
//
// The first is classification. dwReserved0 only holds a reparse tag once
// FILE_ATTRIBUTE_REPARSE_POINT says there is one; read unconditionally it would
// paint arbitrary files cyan, and accepting every tag would claim OneDrive
// placeholders and the WindowsApps stubs as links.
//
// The second is the payload parse. FSCTL_GET_REPARSE_POINT hands back offsets
// the filesystem chose, into a buffer this process owns, and the two link
// layouts do not agree on where the names start -- a symlink carries a Flags
// field a junction does not. Every bound is therefore checked rather than
// trusted, and what is checked here is that a malformed payload is refused
// instead of read past.
//
// The third is the fit. The target shares a pane with the name, so it is
// trimmed from the left to keep the tail -- and the trimmed result must still
// land inside the column budget it was given, since the row it is drawn into is
// bounded by the pane divider rather than by the string.
#define main drift_application_main
#include "../src/drift.c"
#undef main

static int test_failures = 0;

static void TestReport(const char* name, bool passed) {
    printf("  %-68s %s\n", name, passed ? "PASS" : "FAIL");
    if (!passed) test_failures++;
}

// ---------------------------------------------------------------------------
// Payload builder -- assembles what the filesystem would return, so the parse
// can be fed shapes a real volume would take a driver to produce
// ---------------------------------------------------------------------------
typedef struct {
    char  bytes[512];
    DWORD length;
} Payload;

// `print` may be NULL for the junctions that store no print name at all
static Payload BuildPayload(DWORD tag, const wchar_t* substitute, const wchar_t* print) {
    Payload p;
    memset(&p, 0, sizeof(p));

    size_t names = sizeof(ReparseHeader);
    if (tag == IO_REPARSE_TAG_SYMLINK) names += REPARSE_SYMLINK_FLAGS_BYTES;

    size_t sub_bytes = wcslen(substitute) * sizeof(wchar_t);
    size_t print_bytes = print != NULL ? wcslen(print) * sizeof(wchar_t) : 0;

    ReparseHeader* head = (ReparseHeader*)p.bytes;
    head->ReparseTag = tag;
    head->SubstituteNameOffset = 0;
    head->SubstituteNameLength = (WORD)sub_bytes;
    head->PrintNameOffset = (WORD)sub_bytes;
    head->PrintNameLength = (WORD)print_bytes;

    memcpy(p.bytes + names, substitute, sub_bytes);
    if (print_bytes > 0) memcpy(p.bytes + names + sub_bytes, print, print_bytes);

    head->ReparseDataLength = (WORD)(names - 8 + sub_bytes + print_bytes);
    p.length = (DWORD)(names + sub_bytes + print_bytes);
    return p;
}

static bool ParsesTo(Payload* p, const char* expected) {
    char out[MAX_PATH];
    if (!ParseLinkTarget(p->bytes, p->length, out, sizeof(out))) return false;
    return strcmp(out, expected) == 0;
}

static bool Refuses(Payload* p) {
    char out[MAX_PATH];
    return !ParseLinkTarget(p->bytes, p->length, out, sizeof(out));
}

// ---------------------------------------------------------------------------
// Verbatim from DrawScreen: where the arrow starts and how many columns it has
// ---------------------------------------------------------------------------
static int ArrowColumn(int name_len) { return COLUMN_DIVIDER_POSITION + 4 + name_len; }
static int ArrowRoom(int name_len, int divider2) {
    return divider2 - ArrowColumn(name_len) - (int)strlen(LINK_ARROW);
}

int main(void) {
    // -----------------------------------------------------------------------
    // Classification
    // -----------------------------------------------------------------------
    WIN32_FIND_DATA entry;

    memset(&entry, 0, sizeof(entry));
    entry.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    entry.dwReserved0 = IO_REPARSE_TAG_SYMLINK; // stale field, no reparse bit
    TestReport("a plain file is not a link, whatever dwReserved0 happens to hold",
               !IsSymlink(&entry));

    memset(&entry, 0, sizeof(entry));
    entry.dwFileAttributes = FILE_ATTRIBUTE_REPARSE_POINT;
    entry.dwReserved0 = IO_REPARSE_TAG_SYMLINK;
    TestReport("a symlink is a link", IsSymlink(&entry));

    entry.dwFileAttributes = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY;
    entry.dwReserved0 = IO_REPARSE_TAG_MOUNT_POINT;
    TestReport("a junction is a link", IsSymlink(&entry));

    entry.dwFileAttributes = FILE_ATTRIBUTE_REPARSE_POINT;
    entry.dwReserved0 = 0x8000001BL; // IO_REPARSE_TAG_APPEXECLINK
    TestReport("an AppExecLink stub is not offered as a link", !IsSymlink(&entry));

    entry.dwReserved0 = 0x80000013L; // IO_REPARSE_TAG_DEDUP
    TestReport("a deduplicated file is not offered as a link", !IsSymlink(&entry));

    // -----------------------------------------------------------------------
    // Colour -- the whole point of the classification above
    // -----------------------------------------------------------------------
    memset(&entry, 0, sizeof(entry));
    entry.dwFileAttributes = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY;
    entry.dwReserved0 = IO_REPARSE_TAG_MOUNT_POINT;
    TestReport("a directory link draws as a link, not as a directory",
               FileColor(&entry) == cyan);

    entry.dwFileAttributes |= FILE_ATTRIBUTE_HIDDEN;
    TestReport("a hidden junction stays dimmed, so C:\\ does not light up",
               FileColor(&entry) == gray);

    memset(&entry, 0, sizeof(entry));
    entry.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    TestReport("an ordinary directory is unaffected", FileColor(&entry) == blue);

    // -----------------------------------------------------------------------
    // Payload parse -- well-formed shapes
    // -----------------------------------------------------------------------
    Payload p;

    p = BuildPayload(IO_REPARSE_TAG_SYMLINK, L"\\??\\C:\\Data", L"C:\\Data");
    TestReport("a symlink reports the name it was created with",
               ParsesTo(&p, "C:\\Data"));

    p = BuildPayload(IO_REPARSE_TAG_MOUNT_POINT, L"\\??\\C:\\ProgramData",
                     L"C:\\ProgramData");
    TestReport("a junction is parsed past the Flags field it does not have",
               ParsesTo(&p, "C:\\ProgramData"));

    p = BuildPayload(IO_REPARSE_TAG_MOUNT_POINT, L"\\??\\C:\\ProgramData", NULL);
    TestReport("a junction with no print name falls back to the NT name",
               ParsesTo(&p, "C:\\ProgramData"));

    p = BuildPayload(IO_REPARSE_TAG_SYMLINK, L"..\\build", L"..\\build");
    TestReport("a relative link keeps the relative target verbatim",
               ParsesTo(&p, "..\\build"));

    // "\??\" is the NT spelling and means nothing to the reader, but a target
    // that is only the prefix has nothing left once it is gone
    p = BuildPayload(IO_REPARSE_TAG_MOUNT_POINT, L"\\??\\", NULL);
    TestReport("a target of nothing but the NT prefix is not stripped to empty",
               ParsesTo(&p, "\\??\\"));

    // -----------------------------------------------------------------------
    // Payload parse -- malformed shapes, which must be refused, not read past
    // -----------------------------------------------------------------------
    p = BuildPayload(IO_REPARSE_TAG_SYMLINK, L"C:\\Data", L"C:\\Data");
    p.length = sizeof(ReparseHeader) - 1;
    TestReport("a payload too short for the header is refused", Refuses(&p));

    // A symlink's names start four bytes further in than a junction's, so a
    // payload holding only the junction-sized fixed part must not be read as
    // one -- the subtraction that bounds the names would borrow
    p = BuildPayload(IO_REPARSE_TAG_SYMLINK, L"C:\\Data", L"C:\\Data");
    p.length = sizeof(ReparseHeader);
    TestReport("a symlink payload short by its Flags field is refused", Refuses(&p));

    p = BuildPayload(IO_REPARSE_TAG_SYMLINK, L"C:\\Data", L"C:\\Data");
    ((ReparseHeader*)p.bytes)->PrintNameLength = (WORD)(p.length + 64);
    TestReport("a name running past the bytes returned is refused", Refuses(&p));

    p = BuildPayload(IO_REPARSE_TAG_SYMLINK, L"C:\\Data", L"C:\\Data");
    ((ReparseHeader*)p.bytes)->PrintNameOffset = 0xFF00;
    TestReport("a name offset past the bytes returned is refused", Refuses(&p));

    p = BuildPayload(IO_REPARSE_TAG_SYMLINK, L"C:\\Data", L"C:\\Data");
    ((ReparseHeader*)p.bytes)->PrintNameLength = 7; // not a whole UTF-16 unit
    TestReport("a name length that is not whole characters is refused", Refuses(&p));

    p = BuildPayload(IO_REPARSE_TAG_SYMLINK, L"", NULL);
    TestReport("a payload naming no target at all is refused", Refuses(&p));

    p = BuildPayload(0xA0000019L, L"\\??\\C:\\Data", L"C:\\Data"); // GLOBAL_REPARSE
    TestReport("a tag that is not a link is refused", Refuses(&p));

    // -----------------------------------------------------------------------
    // Fitting the target into what the pane has left
    // -----------------------------------------------------------------------
    char fitted[MAX_PATH];

    TestReport("a target that fits is shown whole",
               FitLinkTarget("..\\build", 20, fitted, sizeof(fitted)) == 8 &&
               strcmp(fitted, "..\\build") == 0);

    TestReport("a target of exactly the room available is not trimmed",
               FitLinkTarget("..\\build", 8, fitted, sizeof(fitted)) == 8 &&
               strcmp(fitted, "..\\build") == 0);

    // One column short is where trimming starts, and the tail is what it keeps
    TestReport("one column short trims the head and keeps the tail",
               FitLinkTarget("..\\build", 7, fitted, sizeof(fitted)) == 7 &&
               strcmp(fitted, "...uild") == 0);

    TestReport("a long target keeps its last components",
               FitLinkTarget("C:\\Users\\spencer\\source\\repos\\drift2\\build", 14,
                             fitted, sizeof(fitted)) == 14 &&
               strcmp(fitted, "...rift2\\build") == 0);

    // Room for the marker and nothing else would say only "truncated", which
    // running into the divider already says
    TestReport("room for the ellipsis alone draws no target",
               FitLinkTarget("C:\\Data", 3, fitted, sizeof(fitted)) == 0 &&
               fitted[0] == '\0');

    TestReport("no room at all draws no target",
               FitLinkTarget("C:\\Data", 0, fitted, sizeof(fitted)) == 0 &&
               fitted[0] == '\0');

    TestReport("negative room -- a name that overran the pane -- draws no target",
               FitLinkTarget("C:\\Data", -12, fitted, sizeof(fitted)) == 0 &&
               fitted[0] == '\0');

    // The buffer is the harder bound of the two, and must win. (`small` would
    // have been the name for it, but rpcndr.h in windows.h defines that.)
    char cramped[8];
    int written = FitLinkTarget("C:\\Users\\spencer\\bin", 64, cramped, (int)sizeof(cramped));
    TestReport("more room than buffer still stays inside the buffer",
               written <= (int)sizeof(cramped) - 1 && strlen(cramped) < sizeof(cramped));

    // -----------------------------------------------------------------------
    // Nothing drawn may cross the pane divider
    // -----------------------------------------------------------------------
    bool inside = true;
    bool ever_drawn = false;
    for (int divider2 = MIN_WINDOW_WIDTH; divider2 <= SECOND_DIVIDER_MAX; divider2++) {
        for (int name_len = 0; name_len < 80; name_len++) {
            int room = ArrowRoom(name_len, divider2);
            int n = FitLinkTarget("C:\\Users\\spencer\\source\\repos\\drift2\\build",
                                  room, fitted, sizeof(fitted));
            if (n == 0) continue;
            ever_drawn = true;
            // The arrow, then the target, all of it left of the divider
            int end = ArrowColumn(name_len) + (int)strlen(LINK_ARROW) + n;
            if (end > divider2) inside = false;
        }
    }
    TestReport("no pane width lets the target run into the divider", inside);
    TestReport("the sweep above actually drew something", ever_drawn);

    // -----------------------------------------------------------------------
    // A name that already fills the pane leaves nothing for an arrow
    // -----------------------------------------------------------------------
    int full = SECOND_DIVIDER_MAX - COLUMN_DIVIDER_POSITION - 4;
    TestReport("a name filling the pane suppresses the arrow entirely",
               FitLinkTarget("C:\\Data", ArrowRoom(full, SECOND_DIVIDER_MAX),
                             fitted, sizeof(fitted)) == 0);

    printf("\n");
    if (test_failures > 0) {
        printf("%d symlink test%s FAILED\n", test_failures,
               test_failures == 1 ? "" : "s");
        return 1;
    }
    printf("All symlink tests passed.\n");
    return 0;
}
