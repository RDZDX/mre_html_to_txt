#include "vmsys.h"
#include "vmio.h"
#include "vmchset.h"
#include "vmstdlib.h"
#include <stdio.h>
#include <string.h>

static VMWCHAR kInputPath[256];
static VMWCHAR kOutputPath[256];
static VMWCHAR kErrputPath[256];

//static const size_t kMaxEpubBytes    = 2500 * 1024; //2800 * 1024      //3250 KB
//static const size_t kMaxOutputBytes  = 2 * 1024 * 1024; //2500 * 1024

static const size_t kMaxEpubBytes   = 768 * 1024; //1024 * 1024      //1500 KB
static const size_t kMaxOutputBytes = 512 * 1024; //1024 * 1024

VMBOOL trigeris = VM_FALSE;

struct TextWriter {
    VMFILE file;
    char buffer[1024];
    size_t used;
    size_t total_written;
    int truncated;
    int io_error;
    int pending_space;
    int pending_newline;
};

struct HtmlStripState {
    char tag_buf[96];
    int tag_len;
    int in_tag;
    int skip_mode;
};

enum SkipMode {
    SKIP_NONE = 0,
    SKIP_STYLE = 1,
    SKIP_SCRIPT = 2,
    SKIP_HEAD = 3,
};

void handle_sysevt(VMINT message, VMINT param);

void log_debug(const char* fmt) {

    VMFILE f = vm_file_open(kErrputPath, MODE_APPEND, FALSE);
    if (f < 0)
        f = vm_file_open(kErrputPath, MODE_CREATE_ALWAYS_WRITE, FALSE);

    if (f < 0)
        return;

    VMUINT nwrite;

    vm_file_write(f, (void*)fmt, strlen(fmt), &nwrite);

    vm_file_close(f);
}

static unsigned char ascii_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c - 'A' + 'a');
    }
    return c;
}

static int ascii_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int ascii_equals(const char *text, const char *value) {
    size_t i = 0;
    while (text[i] && value[i]) {
        if (ascii_lower((unsigned char)text[i]) != ascii_lower((unsigned char)value[i])) {
            return 0;
        }
        ++i;
    }
    return text[i] == '\0' && value[i] == '\0';
}

static int is_block_tag(const char *name) {
    if (ascii_equals(name, "br") || ascii_equals(name, "p") || ascii_equals(name, "div") ||
        ascii_equals(name, "li") || ascii_equals(name, "tr") || ascii_equals(name, "td") ||
        ascii_equals(name, "th") || ascii_equals(name, "table") || ascii_equals(name, "section") ||
        ascii_equals(name, "article") || ascii_equals(name, "blockquote") ||
        ascii_equals(name, "body")) {
        return 1;
    }
    return name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0';
}

static int writer_flush(TextWriter *writer) {
    if (!writer || writer->io_error || writer->used == 0) {
        return writer && !writer->io_error;
    }
    VMUINT written = 0;
    if (vm_file_write(writer->file, writer->buffer, (VMUINT)writer->used, &written) < 0 ||
        written != writer->used) {
        writer->io_error = 1;
        return 0;
    }
    writer->total_written += writer->used;
    writer->used = 0;
    return 1;
}

static int writer_put_raw_byte(TextWriter *writer, unsigned char byte) {
    if (!writer || writer->io_error) {
        return 0;
    }
    if (writer->truncated) {
        return 1;
    }
    if (writer->total_written + writer->used >= kMaxOutputBytes) {
        writer->truncated = 1;
        return 1;
    }
    if (writer->used >= sizeof(writer->buffer) && !writer_flush(writer)) {
        return 0;
    }
    writer->buffer[writer->used++] = (char)byte;
    return 1;
}

static int writer_emit_pending(TextWriter *writer) {
    if (!writer || writer->io_error) {
        return 0;
    }
    if (writer->pending_newline) {
        writer->pending_newline = 0;
        writer->pending_space = 0;
        if (writer->total_written > 0 || writer->used > 0) {
            return writer_put_raw_byte(writer, '\n');
        }
        return 1;
    }
    if (writer->pending_space) {
        writer->pending_space = 0;
        if (writer->total_written > 0 || writer->used > 0) {
            return writer_put_raw_byte(writer, ' ');
        }
    }
    return 1;
}

static int writer_append_ascii_char(TextWriter *writer, unsigned char c) {
    if (ascii_is_space(c)) {
        if (c == '\r' || c == '\n') {
            writer->pending_newline = 1;
            writer->pending_space = 0;
        } else if (!writer->pending_newline) {
            writer->pending_space = 1;
        }
        return 1;
    }
    if (!writer_emit_pending(writer)) {
        return 0;
    }
    return writer_put_raw_byte(writer, c);
}

static int writer_append_utf8(TextWriter *writer, unsigned int codepoint) {
    unsigned char utf8[4];
    int count = 0;
    if (codepoint <= 0x7F) {
        return writer_append_ascii_char(writer, (unsigned char)codepoint);
    }
    if (codepoint <= 0x7FF) {
        utf8[0] = (unsigned char)(0xC0 | (codepoint >> 6));
        utf8[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
        count = 2;
    } else if (codepoint <= 0xFFFF) {
        utf8[0] = (unsigned char)(0xE0 | (codepoint >> 12));
        utf8[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
        count = 3;
    } else if (codepoint <= 0x10FFFF) {
        utf8[0] = (unsigned char)(0xF0 | (codepoint >> 18));
        utf8[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        utf8[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
        count = 4;
    } else {
        return 0;
    }
    if (!writer_emit_pending(writer)) {
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        if (!writer_put_raw_byte(writer, utf8[i])) {
            return 0;
        }
    }
    return 1;
}

static int writer_append_data(TextWriter *writer, const unsigned char *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (c < 0x80) {
            if (!writer_append_ascii_char(writer, c)) {
                return 0;
            }
        } else {
            if (!writer_emit_pending(writer) || !writer_put_raw_byte(writer, c)) {
                return 0;
            }
        }
    }
    return 1;
}

static void writer_newline(TextWriter *writer) {
    if (writer) {
        writer->pending_newline = 1;
        writer->pending_space = 0;
    }
}

static int entity_match(const char *entity, size_t len, const char *name) {
    size_t i = 0;
    while (i < len && name[i]) {
        if (ascii_lower((unsigned char)entity[i]) != ascii_lower((unsigned char)name[i])) {
            return 0;
        }
        ++i;
    }
    return i == len && name[i] == '\0';
}

static int decode_entity(TextWriter *writer, const unsigned char *entity, size_t len) {
    if (len == 0) {
        return 0;
    }
    if (entity[0] == '#') {
        unsigned int value = 0;
        size_t i = 1;
        int hex = 0;
        if (i < len && (entity[i] == 'x' || entity[i] == 'X')) {
            hex = 1;
            ++i;
        }
        for (; i < len; ++i) {
            unsigned char c = entity[i];
            if (hex) {
                if (c >= '0' && c <= '9') value = (value << 4) + (c - '0');
                else if (c >= 'a' && c <= 'f') value = (value << 4) + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') value = (value << 4) + (c - 'A' + 10);
                else return 0;
            } else {
                if (c < '0' || c > '9') return 0;
                value = value * 10 + (c - '0');
            }
        }
        if (value == 10 || value == 13) {
            writer_newline(writer);
            return 1;
        }
        if (value == 9 || value == 32) {
            writer->pending_space = 1;
            return 1;
        }
        return writer_append_utf8(writer, value);
    }

    if (entity_match((const char *)entity, len, "amp")) return writer_append_ascii_char(writer, '&');
    if (entity_match((const char *)entity, len, "lt")) return writer_append_ascii_char(writer, '<');
    if (entity_match((const char *)entity, len, "gt")) return writer_append_ascii_char(writer, '>');
    if (entity_match((const char *)entity, len, "nbsp")) {
        writer->pending_space = 1;
        return 1;
    }
    if (entity_match((const char *)entity, len, "quot")) return writer_append_ascii_char(writer, '"');
    if (entity_match((const char *)entity, len, "apos")) return writer_append_ascii_char(writer, '\'');
    return 0;
}

static void finish_tag(HtmlStripState *state, TextWriter *writer) {
    char name[20];
    int i = 0;
    int j = 0;
    int is_end = 0;

    while (i < state->tag_len && ascii_is_space((unsigned char)state->tag_buf[i])) {
        ++i;
    }
    if (i < state->tag_len && state->tag_buf[i] == '/') {
        is_end = 1;
        ++i;
        while (i < state->tag_len && ascii_is_space((unsigned char)state->tag_buf[i])) {
            ++i;
        }
    }
    while (i < state->tag_len && j + 1 < (int)sizeof(name)) {
        unsigned char c = ascii_lower((unsigned char)state->tag_buf[i]);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '!' || c == '?') {
            name[j++] = (char)c;
            ++i;
        } else {
            break;
        }
    }
    name[j] = '\0';

    if (state->skip_mode != SKIP_NONE) {
        if (is_end &&
            ((state->skip_mode == SKIP_STYLE && ascii_equals(name, "style")) ||
             (state->skip_mode == SKIP_SCRIPT && ascii_equals(name, "script")) ||
             (state->skip_mode == SKIP_HEAD && ascii_equals(name, "head")))) {
            state->skip_mode = SKIP_NONE;
        }
        return;
    }

    if (!is_end) {
        if (ascii_equals(name, "style")) {
            state->skip_mode = SKIP_STYLE;
            return;
        }
        if (ascii_equals(name, "script")) {
            state->skip_mode = SKIP_SCRIPT;
            return;
        }
        if (ascii_equals(name, "head")) {
            state->skip_mode = SKIP_HEAD;
            return;
        }
    }

    if (is_block_tag(name)) {
        writer_newline(writer);
    }
}

static int strip_html_to_writer(const unsigned char *html, size_t html_size, TextWriter *writer) {
    HtmlStripState state;
    memset(&state, 0, sizeof(state));

    for (size_t i = 0; i < html_size; ++i) {
        unsigned char c = html[i];
        if (state.in_tag) {
            if (c == '>') {
                finish_tag(&state, writer);
                state.in_tag = 0;
                state.tag_len = 0;
            } else if (state.tag_len + 1 < (int)sizeof(state.tag_buf)) {
                state.tag_buf[state.tag_len++] = (char)c;
            }
            continue;
        }

        if (state.skip_mode != SKIP_NONE) {
            if (c == '<') {
                state.in_tag = 1;
                state.tag_len = 0;
            }
            continue;
        }

        if (c == '<') {
            state.in_tag = 1;
            state.tag_len = 0;
            continue;
        }

        if (c == '&') {
            size_t start = i + 1;
            size_t end = start;
            while (end < html_size && end - start < 16 && html[end] != ';' &&
                   html[end] != '<' && html[end] != '&') {
                ++end;
            }
            if (end < html_size && html[end] == ';' && decode_entity(writer, html + start, end - start)) {
                i = end;
                continue;
            }
        }

        if (!writer_append_data(writer, &c, 1)) {
            return 0;
        }
    }

    writer_newline(writer);
    return writer_emit_pending(writer) && writer_flush(writer);
}

static int convert_html_to_txt(int *truncated)
{
    VMFILE in_file = -1;
    VMFILE out_file = -1;
    unsigned char *html = 0;
    TextWriter writer;
    VMUINT file_size = 0;
    VMUINT read_count = 0;

    *truncated = 0;
    memset(&writer, 0, sizeof(writer));

    in_file = vm_file_open(kInputPath, MODE_READ, VM_TRUE);

    if (in_file < 0) {
        log_debug("Cannot open HTML\n");
        return 0;
    }

    if (vm_file_getfilesize(in_file, &file_size) != 0) {
        log_debug("Cannot get file size\n");
        vm_file_close(in_file);
        return 0;
    }

    if (file_size == 0 || file_size > kMaxEpubBytes) {
        log_debug("HTML too large\n");
        vm_file_close(in_file);
        return 0;
    }

    html = (unsigned char*)vm_malloc(file_size);

    if (!html) {
        log_debug("No RAM for HTML\n");
        vm_file_close(in_file);
        return 0;
    }

    if (vm_file_read(in_file, html, file_size, &read_count) < 0 ||
        read_count != file_size) {

        log_debug("Read failed\n");
        vm_free(html);
        vm_file_close(in_file);
        return 0;
    }

    vm_file_close(in_file);

    out_file = vm_file_open(kOutputPath,
                            MODE_CREATE_ALWAYS_WRITE,
                            VM_TRUE);

    if (out_file < 0) {
        log_debug("Cannot create TXT\n");
        vm_free(html);
        return 0;
    }

    writer.file = out_file;

    if (!strip_html_to_writer(html,
                              (size_t)file_size,
                              &writer)) {

        log_debug("Conversion failed\n");

        vm_free(html);
        vm_file_close(out_file);

        return 0;
    }

    writer_flush(&writer);

    *truncated = writer.truncated;

    if (writer.io_error) {
        log_debug("TXT write failed\n");

        vm_free(html);
        vm_file_close(out_file);

        return 0;
    }

    vm_free(html);
    vm_file_close(out_file);

    return 1;
}

static void start_conversion(void) {

    int truncated = 0;

    if (convert_html_to_txt(&truncated)) {
        if (truncated) {
            //set_status("Done! (512KB cap)", kOutputPath);
        } else {
            //set_status("Done!", kOutputPath);
        }
    } else {
        //set_status("Error");
    }
}

VMINT job(VMWCHAR *file_path, VMINT wlen)
{
    if (!file_path)
        return VM_SELECTOR_ERR_PARAM;

    VMINT len = vm_wstrlen(file_path);

    if (len >= 256)
        return VM_SELECTOR_ERR_PARAM;

    vm_wstrcpy(kInputPath, file_path);

    VMINT cut_len = -1;

    if (len >= 6 && vm_wstrcmp(file_path + len - 5, (VMWSTR)L".html") == 0)
    {
        cut_len = len - 5;
    }
    else if (len >= 5 && vm_wstrcmp(file_path + len - 4, (VMWSTR)L".htm") == 0)
    {
        cut_len = len - 4;
    }
    else if (len >= 7 && vm_wstrcmp(file_path + len - 6, (VMWSTR)L".xhtml") == 0)
    {
        cut_len = len - 6;
    }

    if (cut_len < 0)
        return VM_SELECTOR_ERR_PARAM;

    vm_wstrncpy(kOutputPath, file_path, cut_len);
    kOutputPath[cut_len] = 0;

    vm_wstrncpy(kErrputPath, file_path, cut_len);
    kErrputPath[cut_len] = 0;

    VMWCHAR ext[8];

    vm_ascii_to_ucs2(ext, sizeof(ext), "txt");
    vm_wstrcat(kOutputPath, ext);

    vm_ascii_to_ucs2(ext, sizeof(ext), "err");
    vm_wstrcat(kErrputPath, ext);

    start_conversion();

    return 0;

}
void vm_main(void) {

   vm_reg_sysevt_callback(handle_sysevt);

}

void handle_sysevt(VMINT message, VMINT param) {

    switch (message) {
        case VM_MSG_CREATE:
        case VM_MSG_ACTIVE:
            break;

        case VM_MSG_PAINT:
            if (trigeris == VM_TRUE) {vm_exit_app();}
            if (vm_selector_run(0, 0, job) == 0) {trigeris = VM_TRUE;}
            break;

        case VM_MSG_INACTIVE:
            break;

        case VM_MSG_QUIT:
            break;
    }
}
