#define XK_MISCELLANY
#define XK_LATIN1
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

// Only trigger hotkeys if window in keyboard focus is one of these.
const char* g_text_editors[] =
{
    "kate",
    "Typora",
};

Display* g_display;
Window   g_root;
Atom     WM_CLASS;

#define fail(...) exit(!!fprintf(stderr, "hotkeys: "__VA_ARGS__))

void* xmalloc(size_t size)
{
    void* p = malloc(size);
    if (p == NULL)
        fail("Out of memory.\n");
    return p;
}

void* xrealloc(void* p, size_t size)
{
    p = realloc(p, size);
    if (p == NULL)
        fail("Out of memory.\n");
    return p;
}

void x_ptr_free(void* ptrptr)
{
    XFree(*(void**)ptrptr);
}
#define XFREE __attribute__((cleanup(x_ptr_free)))

void data_free(Data* data)
{
    free(data->data);
}
#define DATA_FREE __attribute__((cleanup(data_free)))

// atexit destructors
void close_display(void)   { XCloseDisplay(g_display); }
void ungrab_keyboard(void) { XUngrabKeyboard(g_display, CurrentTime); }

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
Data peek_selection(Window window, Data* optional_buf)
{
    DATA_FREE Data clipboard_backup = get_clipboard(NULL);

    send_key(window, XK_c, ControlMask);
    XFlush(g_display);
    usleep(10); // wait for copy to happen

    Data clipboard = get_clipboard(optional_buf);
    set_clipboard(clipboard_backup);

    return clipboard;
}

bool key_down(char keymap[32], KeySym key)
{
    KeyCode keycode = XKeysymToKeycode(g_display, key);
    return keymap[keycode >> 3] & (1 << (keycode&7));
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
        XK_f, XK_t,
    };
    for (size_t i = 0; i < sizeof keys_to_grab / sizeof keys_to_grab[0]; ++i) {
        XGrabKey(
            g_display,
            XKeysymToKeycode(g_display, keys_to_grab[i]), 0,
            g_root, False, GrabModeAsync, GrabModeSync);
    }

    enum {
        LEFT  = -1,
        NONE  =  0,
        RIGHT = +1,
    } selecting = NONE;
    bool inclusive = false; // e.g. 't' or 'f' when finding character

    while (true) {
        XEvent event;
        XNextEvent(g_display, &event);
        if (event.type != KeyPress && event.type != KeyRelease)
            continue;

        KeySym key = XKeycodeToKeysym(g_display, event.xkey.keycode, 0);
        if (key == XK_Escape) // temporary convenience while developing
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
            continue;
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
            continue;
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
            continue;
        }

        if ( ! selecting) {
            if (event.type == KeyPress) {
                char keymap[32];
                XQueryKeymap(g_display, keymap);

                if ( ! (key_down(keymap, XK_Left) ^ key_down(keymap, XK_Right))) {
                    XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
                    continue;
                }
                XAllowEvents(g_display, AsyncKeyboard, CurrentTime);
                XFlush(g_display);

                inclusive = XKeycodeToKeysym(g_display, event.xkey.keycode, 0) == XK_f;
                selecting = key_down(keymap, XK_Left) ? LEFT : RIGHT;
                send_key(focused, selecting == LEFT ? XK_Right : XK_Left, 0); // undo cursor move
                send_key(focused, selecting == LEFT ? XK_Home : XK_End, ShiftMask);
            } else { // key release
                XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
            }
        } else { // selecting
            if (event.type == KeyPress) { // repeat triggered or smashing keys
                XAllowEvents(g_display, ReplayKeyboard, CurrentTime);
            } else { // key release
                DATA_FREE Data selection = peek_selection(focused, NULL);
                if (selection.length == 0 || selection.data[0] == '\n')
                    goto skip_find;

                send_key(focused, selecting == LEFT ? XK_Right : XK_Left, 0);

                int grab_status = XGrabKeyboard(
                    g_display, g_root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
                if (grab_status == GrabSuccess) { // get key to find
                    XEvent event;
                    KeySym keysym;
                    char character[32] = "";
                    Status status;
                    while (true) {
                        XNextEvent(g_display, &event);
                        if (event.type != KeyPress) // remember that release for
                            continue;               // Xutf8Lookup is undefined.

                        keysym = XKeycodeToKeysym(g_display, event.xkey.keycode, 0);

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

                        Xutf8LookupString(
                            input_context,
                            &event.xkey,
                            character, sizeof character,
                            &keysym, &status);

                        // Only break if got character, so ignore Shift and stuff.
                        if (status == XLookupChars || status == XLookupBoth)
                            break;
                    }
                    XUngrabKeyboard(g_display, CurrentTime);
                    XFlush(g_display);
                    printf("Finding key '%s' from string %s\n",
                           character,
                           selection.data);

                    char* position = strstr(selection.data, character);
                    if (selecting == LEFT) // find last
                        for (char* p = position; p != NULL;)
                            if (((p = strstr(position + 1, character)) != NULL))
                                position = p;

                    if (position == NULL)
                        goto skip_find;

                    if (selecting == RIGHT)
                        for (ptrdiff_t i = 0; i < position - selection.data + inclusive; i++)
                            send_key(focused, XK_Right, ShiftMask);
                    else
                        for (size_t i = 0;
                             i + 1 < selection.length - (position - selection.data) + inclusive;
                            i++
                        )
                            send_key(focused, XK_Left, ShiftMask);
                }
                skip_find:
                XAllowEvents(g_display, AsyncKeyboard, CurrentTime);
            }
            selecting = NONE;
        }

        XFlush(g_display);
    }
}
