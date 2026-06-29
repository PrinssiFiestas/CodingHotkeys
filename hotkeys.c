#define XK_LATIN1
#define XK_MISCELLANY
#define XK_XKB_KEYS // XKB extension for AltGr
#include <X11/Xlib.h>
#include <X11/keysymdef.h>

#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>

// XKeycodeToKeysym() seems to be tagged as deprecated despite no mentions in
// manual pages about deprecation.
#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#else
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

typedef struct
{
    size_t length;
    size_t capacity;
    char*  data;
} Data;

// Negatives go back (line/column decreases), positives go forward (line/column
// increases).
enum selecting_info {
    LEFT_BRACKET  = -3,
    UP            = -2,
    LEFT          = -1,
    NONE          =  0,
    RIGHT         = +1,
    DOWN          = +2,
    RIGHT_BRACKET = +3,
};

// Absolute values of the enum above.
#define SAME_LINE  1
#define OTHER_LINE 2
#define BRACKET    3

// Only trigger hotkeys if window in keyboard focus is one of these.
const char* g_text_editors[] =
{
    "kate",
    "Typora",
    "Brave-browser", // mostly for Shadertoy
};

// Least significant bit of index controls if left or right.
static const char* BRACKETS[] = {
    "(", ")",
    "[", "]",
    "{", "}",
    "<", ">",
    "'", "'",
    "\"", "\"",
};
#define RIGHT_BIT 1

// Analyzing multiple lines at once on multi-line operations is faster, but may
// introduce annoying scrolling.
#define LINES_TO_ANALYZE 7

Display* g_display;
Window   g_root;
Atom     WM_CLASS;

#define fail(...) exit(!!fprintf(stderr, "hotkeys: "__VA_ARGS__))

void* xmalloc(size_t size)
{
    void* p = malloc(size);
    if (p == NULL)
        fail("malloc(): %s.\n", strerror(errno));
    return p;
}

void* xrealloc(void* p, size_t size)
{
    p = realloc(p, size);
    if (p == NULL)
        fail("realloc(): %s.\n", strerror(errno));
    return p;
}

void x_ptr_free(void* ptrptr)
{
    XFree(*(void**)ptrptr);
}
#define XFREE __attribute__((cleanup(x_ptr_free)))

Data data_clone(Data data)
{
    Data new_data = {
        .length   = data.length,
        .capacity = data.length + sizeof"",
        .data     = xmalloc(data.length + sizeof""),
    };
    memcpy(new_data.data, data.data, data.length + sizeof"");
    return new_data;
}

bool data_equal(Data a, Data b)
{
    return a.length == b.length && memcmp(a.data, b.data, a.length) == 0;
}

void data_free(Data* data)
{
    free(data->data);
}
#define DATA_FREE __attribute__((cleanup(data_free)))

// atexit destructor
void close_display(void)
{
    XCloseDisplay(g_display);
}

void send_key_press(Window window, KeySym key, unsigned mask)
{
    XEvent event = {
        .xkey.type      = KeyPress,
        .xkey.window    = window,
        .xkey.keycode   = XKeysymToKeycode(g_display, key),
        .xkey.state     = mask,
    };

    XSendEvent(g_display, window, False, KeyPressMask | KeyReleaseMask, &event);
}

void send_key_release(Window window, KeySym key, unsigned mask)
{
    XEvent event = {
        .xkey.type      = KeyRelease,
        .xkey.window    = window,
        .xkey.keycode   = XKeysymToKeycode(g_display, key),
        .xkey.state     = mask,
    };

    XSendEvent(g_display, window, False, KeyPressMask | KeyReleaseMask, &event);
}

void send_key(Window window, KeySym key, unsigned mask)
{
    send_key_press(window, key, mask);
    send_key_release(window, key, mask);
}

// This is mostly used with async grabs, sync grabs should forward using
// XAllowEvents().
void forward_key(Window window, XEvent event)
{
    event.xkey.window    = window;
    event.xkey.subwindow = None;
    event.xkey.time      = CurrentTime;

    XSendEvent(g_display, window, False, KeyPressMask | KeyReleaseMask, &event);
    XFlush(g_display);
}

// NOTE: It is better to use xclip than handling clipboard ourselves. This is
// because it can handle other data types than strings as well, so we get a much
// more reliable clipboard backup. It's anyway easier, albeit slow.

// optional_buf must be allocated with malloc() or NULL. This uses xclip to
// be able to receive arbitrary data like ClipboardAll in AutoHotkey.
Data get_clipboard(Data* optional_buf)
{
    FILE* fp = popen("xclip -selection clipboard", "r");
    if (fp == NULL)
        fail("Could not execute xclip: %s\n", strerror(errno));

    Data* data = optional_buf != NULL ?
        optional_buf
      : &(Data){.capacity = 32, .data = xmalloc(32) };

    data->length = 0;
    while (true) {
        size_t bytes_read = fread(
            data->data + data->length, sizeof(char), data->capacity - data->length, fp);
        data->length += bytes_read;
        if (bytes_read < data->capacity - data->length - bytes_read && feof(fp))
            break;
        data->data = xrealloc(data->data, data->capacity <<= 1);
    }
    data->data[data->length] = '\0';

    pclose(fp);
    return *data;
}

void set_clipboard(Data data)
{
    FILE* fp = popen("xclip -selection clipboard", "w");
    if (fp == NULL)
        fail("Could not execute xclip: %s\n", strerror(errno));

    fwrite(data.data, sizeof(char), data.length, fp);
    pclose(fp);
}

// Kate doesn't seem to claim PRIMARY ownership when selecting text using
// synthetic key events, so we'll explicitly copy instead, but we have to
// backup clipboard and store it back, so our peeking doesn't interfere with
// clipboard.
//
// Because we are sending Ctrl+C, we should make sure to call this only if
// something is selected, otherwise the contents will be old junk.
Data peek_selection(Window window, Data* optional_buf)
{
    DATA_FREE Data clipboard_backup = get_clipboard(NULL);

    send_key(window, XK_c, ControlMask);
    XFlush(g_display);
    usleep(10*1000); // wait for copy to happen
    usleep(1000*1000); // TODO TEMP

    Data clipboard = get_clipboard(optional_buf);
    set_clipboard(clipboard_backup);

    return clipboard;
}

bool key_down(char keymap[32], KeySym key)
{
    KeyCode keycode = XKeysymToKeycode(g_display, key);
    return keymap[keycode >> 3] & (1 << (keycode&7));
}

// UTF-8 codepoint size. We should handle graphemes instead, but this is simpler
// and usually good enough.
size_t char_size(const char* _ptr)
{
    const unsigned char* ptr = (unsigned char*)_ptr;
    static const unsigned sizes[] = {
        1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
        0,0,0,0,0,0,0,0, 2,2,2,2,3,3,4,0 };
    return sizes[*ptr >> 3];
}

// UTF-8 codepoint size of the leftmost character.
size_t char_size_back(const char* ptr)
{
    const char* p1 = ptr - 1;
    while (char_size(p1) == 0)
        p1--;
    return ptr - p1;
}

// Time complexity: O(don't care)
char* strstr_last(const char* haystack, const char* needle, const char* optional_end)
{
    const char* position = strstr(haystack, needle);
    for (const char* p = position; p != NULL; )
        if (((p = strstr(position + strlen(needle), needle)) != NULL))
            position = p;
    if (optional_end != NULL && position >= optional_end)
        return NULL;
    return (char*)position;
}

char* find_above_or_below(
    Window window,
    Data* selection,
    const char* utf8_character,
    enum selecting_info selecting,
    bool inclusive_match) // ignore the bracket that we are matching to
{
    const char* left  = NULL;
    const char* right = NULL;
    for (size_t i = 0; i < sizeof BRACKETS / sizeof BRACKETS[0]; i++) {
        if (utf8_character[0] != BRACKETS[i][0])
            continue;
        left  = BRACKETS[i & ~RIGHT_BIT];
        right = BRACKETS[i |  RIGHT_BIT];
        break;
    }

    // Wait for all keys to be lifted. We already waited before calling this in
    // event loop, but the event loop may fail waiting due to key repeat
    // triggering release events.
    while (true) {
        char keymap[32];
        XQueryKeymap(g_display, keymap);
        if (memcmp(keymap, (char[sizeof keymap]){0}, sizeof keymap) == 0)
            break;
    }

    char* position = NULL;
    while (position == NULL) {
        DATA_FREE Data old_selection = data_clone(*selection);
        for (size_t i = 0; i < LINES_TO_ANALYZE; i++)
            send_key(window, selecting <= UP ? XK_Up : XK_Down, ShiftMask);

        peek_selection(window, selection);
        if (data_equal(*selection, old_selection)) { // end of file
            puts("End of file.");
            return NULL;
        }

        // Our string processing is horribly inefficient, whatever.

        size_t nesting = -inclusive_match;
        switch (selecting) {
        case DOWN:
            position = strstr(selection->data, utf8_character);
            break;
        case UP:
            position = strstr_last(selection->data, utf8_character, NULL);
            break;

        case RIGHT_BRACKET:
            for (char* p = selection->data; *p != '\0'; ) {
                if (strncmp(p, left, strlen(left)) == 0) {
                    nesting++;
                    p += strlen(left);
                } else if (strncmp(p, right, strlen(right)) == 0) {
                    if ( ! nesting) {
                        position = p;
                        break;
                    }
                    nesting--;
                    p += strlen(right);
                } else
                    p++;
            }
            break;

        case LEFT_BRACKET: // TODO looking from the last character, but we should
                           // use a similar method to strstr_last() instead.
            for (char* p = selection->data + selection->length - 1; p >= selection->data; ) {
                if (strncmp(p, right, strlen(right)) == 0) {
                    nesting++;
                    p -= strlen(right);
                } else if (strncmp(p, left, strlen(left)) == 0) {
                    if ( ! nesting) {
                        position = p;
                        break;
                    }
                    nesting--;
                    p -= strlen(left);
                } else
                    p--;
            }
            break;

        case LEFT ... RIGHT:
            fail("Unreachable. Line %i.\n", __LINE__);
        }

        if (position != NULL) {
            printf("Found character '%s'\n", utf8_character);
            printf("Nesting level: %zi\n", nesting);
            return position;
        }

        char keymap[32];
        XQueryKeymap(g_display, keymap);
        if (key_down(keymap, XK_Escape)) {
            send_key(window, selecting <= UP ? XK_Right : XK_Left, 0);
            puts("Aborting search.");
            return NULL;
        }
        // Keyboard not grabbed, so the only sensible thing to do if any key
        // has been pressed is to abort search. Grabbing would work better, but
        // it's heavy handed, this is an edge case anyway (user panic).
        if (memcmp(keymap, (char[sizeof keymap]){0}, sizeof keymap) != 0)
            return NULL;
    }
    fail("Unreachable. Line %i.\n", __LINE__);
    return position;
}

int main(void)
{
    g_display = XOpenDisplay(NULL);
    assert(g_display != NULL);
    atexit(close_display);

    g_root = DefaultRootWindow(g_display);

    WM_CLASS = XInternAtom(g_display, "WM_CLASS", False);

    // Stuff for Xutf8LookupString().
    XIM input_method = XOpenIM(g_display, NULL, NULL, NULL);
    if (input_method == NULL)
        fail("Failed to open input method.\n");
    XIC input_context = XCreateIC(
        input_method,
        XNInputStyle,
        XIMPreeditNothing | XIMStatusNothing,
        XNClientWindow,
        g_root, NULL);
    if (input_context == NULL)
        fail("Failed to create input context.\n");

    // NOTE: Arrow keys are not grabbed. This is because there is no
    // reliable way of forwarding them to applications (Kate specifically)
    // without breaking key repeat. We use key_down() instead for arrow keys.
    KeySym keys_to_grab[] = {
        XK_Escape, // TODO temp exit while developing.
        XK_f, XK_t, // find character on left, right, above, and below
        XK_section, XK_aring, // '§' and 'å' as custom modifiers
        XK_i, XK_a, // inner and outer (all)
    };
    for (size_t i = 0; i < sizeof keys_to_grab / sizeof keys_to_grab[0]; ++i) {
        XGrabKey(
            g_display,
            XKeysymToKeycode(g_display, keys_to_grab[i]), 0,
            g_root, False, GrabModeAsync, GrabModeSync);
    }

    enum selecting_info selecting = NONE;
    bool inclusive = false; // e.g. 't' or 'f' when finding character
    bool alternative_modifier_down = false; // not Alt, but our custom modifier

    // ------------------------------------------------------------------------
    main_event_loop:;

    XEvent event;
    XNextEvent(g_display, &event);
    if (event.type != KeyPress && event.type != KeyRelease)
        goto main_event_loop;

    KeySym key = XKeycodeToKeysym(g_display, event.xkey.keycode, 0);
    if (key == XK_Escape) // TODO temporary convenience while developing
        exit(EXIT_SUCCESS);
    printf("Key %s: Keycode=%i, Keysym=%s\n", // TODO temp logging.
        event.type == KeyPress ? "pressed" : "released",
        event.xkey.keycode,
        XKeysymToString(key));

    XFREE char* focused_instance;
    Window focused;
    XGetInputFocus(g_display, &focused, &(int){0}); // ignore revert flag
    if (focused == None) {
        XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
        goto main_event_loop;
    }
    XGetWindowProperty(
        g_display,
        focused,
        WM_CLASS,
        0, LONG_MAX, False,
        AnyPropertyType,
        &(Atom){0}, // ignore type, we know it's a string
        &(int){0}, // ignore format, we know it's a string
        &(unsigned long){0}, // ignore length, string null terminated
        &(unsigned long){0}, // ignore partial read, we don't care
        (unsigned char**)&focused_instance);

    if (focused_instance == NULL) {
        XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
        goto main_event_loop;
    }
    char* focused_class = focused_instance;
    focused_class += strlen(focused_instance) + sizeof"";
    bool found_focused = false;
    for (size_t i = 0; i < sizeof g_text_editors / sizeof g_text_editors[0]; ++i) {
        if (strcmp(focused_class, g_text_editors[i]) == 0) {
            found_focused = true;
            break;
        }
    }
    if ( ! found_focused) {
        XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
        goto main_event_loop;
    }

    if ( ! selecting) {
        if (event.type == KeyPress) {
            switch(key) {
            // FIXME: both modifiers would need their own flags, this breaks
            // if both are down simultaneously.
            case XK_section: case XK_aring:
                alternative_modifier_down = true;
                XAllowEvents(g_display, AsyncKeyboard, CurrentTime);
                XFlush(g_display);
                goto main_event_loop;

            case XK_i: case XK_a:
                if (alternative_modifier_down) {
                    selecting = BRACKET; // direction determined later
                    inclusive = key == XK_a;
                    XAllowEvents(g_display, AsyncKeyboard, CurrentTime);
                } else
                    XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
                XFlush(g_display);
                goto main_event_loop;
            }

            char keymap[32];
            XQueryKeymap(g_display, keymap);

            switch ( // make sure that only one arrow pressed.
                (key_down(keymap, XK_Up   ) << 0) |
                (key_down(keymap, XK_Left ) << 1) |
                (key_down(keymap, XK_Right) << 2) |
                (key_down(keymap, XK_Down ) << 3) )
            {
            case 1 << 0: selecting = UP   ; break;
            case 1 << 1: selecting = LEFT ; break;
            case 1 << 2: selecting = RIGHT; break;
            case 1 << 3: selecting = DOWN ; break;
            default: // multiple arrows pressed
                XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
                goto main_event_loop;
            }

            XAllowEvents(g_display, AsyncKeyboard, CurrentTime);
            XFlush(g_display);

            inclusive = key == XK_f;

            switch (selecting) { // undo arrow press / cursor move
            case UP:    send_key(focused, XK_Down,  0); break;
            case LEFT:  send_key(focused, XK_Right, 0); break;
            case RIGHT: send_key(focused, XK_Left,  0); break;
            case DOWN:  send_key(focused, XK_Up,    0); break;
            case NONE: case LEFT_BRACKET: case RIGHT_BRACKET:
                fail("Unreachable. Line %i\n", __LINE__);
            }
            send_key(focused, selecting <= LEFT ? XK_Home : XK_End, ShiftMask);
        } else { // key release
            if (key == XK_section || key == XK_aring) {
                alternative_modifier_down = false;
                XAllowEvents(g_display, AsyncKeyboard, CurrentTime);
            } else
                XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
        }
    } else { // selecting
        alternative_modifier_down = false; // release event above skipped, set manually

        if (event.type == KeyPress) { // key repeat or smashing keys
            XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
            selecting = NONE;
            goto main_event_loop;
        } // else key release

        // Kate copies line if no selection and peek_selection() sends Ctrl+C.
        // Nothing is selected if finding brackets, so initialize accordingly.
        DATA_FREE Data selection = selecting != BRACKET ?
            peek_selection(focused, NULL)
          : (Data){.capacity = 1, .data = memset(xmalloc(1), 0, 1) };

        // Unselect line and get back to starting position.
        if (selecting != BRACKET)
            send_key(focused, selecting <= LEFT ? XK_Right : XK_Left, 0);

        int grab_status = XGrabKeyboard(
            g_display, g_root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
        if (grab_status != GrabSuccess) // probably somebody else grabbing.
            goto skip_find;

        char utf8_character[32] = "";

        // find_above()/below() requires that no key is held down while it
        // does it's thing. Using XQueryKeymap() alone is not reliable, it
        // may lag, so we check press/release events here instead and use
        // XQueryKeymap() during finding operation.
        bool keys_down[(KeyCode)-1] = {0};
        // Checking if any key down using keycodes should be enough, so
        // checking modifier mask too is a bit paranoid, but this double
        // check makes sure that no modifiers really really are down before
        // finding above/below, which would cause mayhem otherwise.
        unsigned modifiers = 0;

        size_t bracket = 0; // index to BRACKETS
        bool got_key = false;

        while (true) {
            XEvent event;
            XNextEvent(g_display, &event);
            if (event.type != KeyPress && event.type != KeyRelease)
                continue;

            if (event.type == KeyRelease) {
                keys_down[event.xkey.keycode] = false;
                modifiers &= ~event.xkey.state;
                if (got_key
                    && modifiers == 0
                    && memcmp(
                        keys_down, (char[sizeof keys_down]){0}, sizeof keys_down) == 0
                    ) {
                    break;
                }
                continue;
            }

            KeySym keysym = XKeycodeToKeysym(g_display, event.xkey.keycode, 0);
            keys_down[event.xkey.keycode] = true;
            modifiers |= event.xkey.state;

            switch (keysym) {
            case XK_F1 ... XK_R15: case XK_Super_L: case XK_Super_R:
            case XK_Left: case XK_Right: case XK_Up: case XK_Down:
            case XK_Home: case XK_End: case XK_Page_Up: case XK_Page_Down:
                // User likely changed their minds and want to do something else.
                XUngrabKeyboard(g_display, CurrentTime);
                XFlush(g_display);
                forward_key(focused, event);
                goto skip_find;

            case XK_Escape: // User likely selected on accident and they
                            // just want to cancel, so don't forward key.
                XUngrabKeyboard(g_display, CurrentTime);
                XFlush(g_display);
                goto skip_find;
            }

            Status status;
            Xutf8LookupString(
                input_context,
                &event.xkey,
                utf8_character, sizeof utf8_character,
                &keysym, &status);

            // We need a fully formed character, try again if key was shift or something.
            if (status != XLookupChars && status != XLookupBoth)
                continue;

            if (selecting == BRACKET) {
                for (; bracket < sizeof BRACKETS / sizeof BRACKETS[0]; bracket++)
                    if (BRACKETS[bracket][0] == utf8_character[0])
                        break;
                if (bracket >= sizeof BRACKETS / sizeof BRACKETS[0]) {
                    XUngrabKeyboard(g_display, CurrentTime);
                    XFlush(g_display);
                    goto skip_find;
                }
                selecting = bracket & RIGHT_BIT ? RIGHT_BRACKET : LEFT_BRACKET;
            }

            got_key = true;

            // Find above, below, and bracket has to wait for all keys to be
            // lifted, but finding in line does not.
            if (abs(selecting) == SAME_LINE)
                break;
        }
        XUngrabKeyboard(g_display, CurrentTime);
        XFlush(g_display);

        printf("Finding key '%s' from string %s\n",
                utf8_character,
                selection.data);

        char* position = strstr(selection.data, utf8_character);
        if (selecting <= LEFT) // find last
            for (char* p = position; p != NULL;)
                if (((p = strstr(position + 1, utf8_character)) != NULL))
                    position = p;

        if (abs(selecting) == BRACKET) { // First find opposing, matching later
            position = find_above_or_below(
                focused, &selection, BRACKETS[bracket ^ RIGHT_BIT], -selecting, false);
            inclusive ^= 1; // see comment below
        } else if (position == NULL && abs(selecting) == OTHER_LINE) {
            position = find_above_or_below(
                focused, &selection, utf8_character, selecting, false);
            // If found above/below instead from line, then we don't select from
            // starting position to end position, we switch directions: start
            // from other side of selection and unselect to position.
            selecting *= -1;
            inclusive ^=  1;
        }
        if (position == NULL)
            goto skip_find;

        select_other: // come back in case we have to select matching bracket.
        switch (selecting) {
        case RIGHT: case DOWN: case RIGHT_BRACKET:
            for (char* p = selection.data;
                p < position + inclusive;
                p += char_size(p)
            ) // TODO sleep and flush
                usleep(10*1000), send_key(focused, XK_Right, ShiftMask), XFlush(g_display);
            break;

        case LEFT: case UP: case LEFT_BRACKET:
            for (char* p = selection.data + selection.length;
                p - 1 > position - inclusive;
                p -= char_size_back(p)
            ) // TODO sleep and flush
                usleep(10*100), send_key(focused, XK_Left, ShiftMask), XFlush(g_display);
            break;

        case NONE:
            fail("Unreachable. Line %i.\n", __LINE__);
        }

        if (abs(selecting) == BRACKET) { // find matching
            puts("Going back.");
            usleep(3000*1000); // TODO

            // Send key to go to the opposite bracket found.
            // Edge case: If starting non-inclusive search right before the bracket
            // to search, then the first selection will end up empty, so we don't
            // want to send key to get there, we already are there. Note that
            // we had to flip the bit before, which is why the check is negated.
            bool go_to_bracket = !inclusive;
            if (!!inclusive)
                go_to_bracket = (selecting == LEFT_BRACKET) ?
                    (position != selection.data)
                  : (position != selection.data + selection.length - 1);
            if (go_to_bracket) {
                send_key(focused, bracket & RIGHT_BIT ? XK_Left : XK_Right, 0);
                XFlush(g_display);
            }
            usleep(400*1000);

            position = find_above_or_below(
                focused, &selection, BRACKETS[bracket], selecting, !inclusive);
            if (position == NULL)
                goto skip_find;

            selecting = bracket & RIGHT_BIT ? LEFT : RIGHT;
            goto select_other;
        }

        skip_find:
        XAllowEvents(g_display, AsyncKeyboard, CurrentTime);
        selecting = NONE;
    }

    XFlush(g_display);
    goto main_event_loop;
}
