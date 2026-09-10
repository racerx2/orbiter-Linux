// Linux replacement for htmlctrl.c.
//
// The Windows original embeds an Internet Explorer OLE object (IWebBrowser2 /
// IHTMLDocument2 / IDocHostUIHandler) in a child window to render scenario
// descriptions. COM and IE have no native Linux existence, so the control is
// reimplemented rather than shimmed.
//
// The public API is unchanged, so Src/Orbiter/TabScenario.cpp compiles and
// calls into this file without modification:
//
//     RegisterHtmlCtrl (HINSTANCE, BOOL)
//     DisplayHTMLPage  (HWND, LPTSTR)
//     DisplayHTMLStr   (HWND, const char *)
//
// WHAT IS PRESERVED
//   The scenario description pane still shows the text of a scenario's
//   BEGIN_HYPERDESC block. Markup is converted to readable plain text:
//   block-level tags become line breaks, character entities are decoded,
//   and runs of whitespace are collapsed the way a browser would collapse
//   them. The three call sites keep working and the pane is never blank.
//
// WHAT IS LOST
//   Formatting. Headings, bold, italics, tables and images render as plain
//   text, and DisplayHTMLPage cannot follow links or load a remote URL --
//   it reads a local file if the path resolves, and otherwise shows the
//   location instead of fetching it.
//
// WHAT WOULD RESTORE IT
//   Rendering the description with a real engine. The realistic options are
//   WebKitGTK (webkit2gtk-4.1, a GtkWidget, which would need a GTK window
//   alongside the GLFW one) or a small HTML-subset renderer drawing into the
//   existing ImGui pane. The latter fits the rest of this port better: the
//   hyperdesc blocks in the stock scenarios use a narrow tag vocabulary
//   (h1-h3, p, b, i, ul/li, br, a), which is well within what an ImGui
//   draw-list renderer can handle without pulling in a browser engine.

#include <windows.h>

#include <string>
#include <fstream>
#include <sstream>
#include <cctype>

namespace {

// Tags that should produce a line break when opened or closed, matching how a
// browser lays out block-level elements.
bool isBlockTag(const std::string &tag)
{
    static const char *block[] = {
        "p", "br", "div", "tr", "li", "ul", "ol", "table",
        "h1", "h2", "h3", "h4", "h5", "h6", "hr", "pre", "blockquote"
    };
    for (const char *b : block)
        if (tag == b) return true;
    return false;
}

// Decodes the entities that appear in the stock scenario descriptions.
// Anything unrecognised is passed through unchanged rather than dropped, so
// no text is silently lost.
std::string decodeEntity(const std::string &ent)
{
    if (ent == "amp")    return "&";
    if (ent == "lt")     return "<";
    if (ent == "gt")     return ">";
    if (ent == "quot")   return "\"";
    if (ent == "apos")   return "'";
    if (ent == "nbsp")   return " ";
    if (ent == "deg")    return "\xc2\xb0";   // UTF-8 degree sign
    if (ent == "hellip") return "...";
    if (ent == "mdash")  return "--";
    if (ent == "ndash")  return "-";
    if (ent.size() > 1 && ent[0] == '#') {
        // Numeric reference. Only Latin-1 is emitted; anything above that is
        // rare in these files and falls through to the literal form.
        int code = 0;
        try {
            code = (ent[1] == 'x' || ent[1] == 'X')
                 ? std::stoi(ent.substr(2), nullptr, 16)
                 : std::stoi(ent.substr(1));
        } catch (...) { return "&" + ent + ";"; }
        if (code > 0 && code < 128) return std::string(1, (char)code);
        if (code < 256) {
            // Encode as two-byte UTF-8.
            std::string s;
            s += (char)(0xC0 | (code >> 6));
            s += (char)(0x80 | (code & 0x3F));
            return s;
        }
    }
    return "&" + ent + ";";
}

// Strips markup, leaving text laid out roughly as a browser would: block
// elements break lines, inline whitespace collapses, and <script>/<style>
// bodies are discarded rather than printed.
std::string htmlToText(const char *html)
{
    if (!html) return std::string();

    std::string out;
    out.reserve(strlen(html));

    bool inTag        = false;
    bool pendingSpace = false;
    std::string tag;
    std::string skipUntil;   // non-empty while inside <script> or <style>

    for (const char *p = html; *p; ++p) {
        char c = *p;

        if (inTag) {
            if (c == '>') {
                inTag = false;

                // Normalise: strip a leading '/', keep only the tag name.
                std::string name;
                size_t i = 0;
                if (i < tag.size() && tag[i] == '/') ++i;
                for (; i < tag.size() && !isspace((unsigned char)tag[i]) && tag[i] != '/'; ++i)
                    name += (char)tolower((unsigned char)tag[i]);

                if (!skipUntil.empty()) {
                    if (name == skipUntil && tag[0] == '/') skipUntil.clear();
                } else if (name == "script" || name == "style") {
                    if (tag[0] != '/') skipUntil = name;
                } else if (isBlockTag(name)) {
                    // Collapse consecutive breaks so nested block tags do not
                    // produce a run of blank lines.
                    if (!out.empty() && out.back() != '\n') out += '\n';
                    pendingSpace = false;
                }
                tag.clear();
            } else {
                tag += c;
            }
            continue;
        }

        if (c == '<') { inTag = true; tag.clear(); continue; }
        if (!skipUntil.empty()) continue;

        if (c == '&') {
            const char *semi = strchr(p, ';');
            if (semi && semi - p <= 10) {
                if (pendingSpace && !out.empty()) { out += ' '; pendingSpace = false; }
                out += decodeEntity(std::string(p + 1, semi));
                p = semi;
                continue;
            }
        }

        if (isspace((unsigned char)c)) {
            // Whitespace collapses, and never leads a line.
            if (!out.empty() && out.back() != '\n') pendingSpace = true;
            continue;
        }

        if (pendingSpace) { out += ' '; pendingSpace = false; }
        out += c;
    }

    // Trim trailing blank lines.
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();

    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API — signatures identical to htmlctrl.h
// ---------------------------------------------------------------------------

extern "C" {

// On Windows this registers the browser host window class. There is no such
// class here: the description pane is an ordinary static control that the
// dialog implementation already knows how to draw, so registration is a no-op.
void RegisterHtmlCtrl(HINSTANCE, BOOL)
{
}

// Loads a local HTML file into the control. A remote URL is not fetched;
// its location is shown instead, so the pane still tells the user what the
// scenario pointed at rather than going blank.
long DisplayHTMLPage(HWND hwnd, LPTSTR webPageName)
{
    if (!hwnd || !webPageName) return -1;

    const std::string path(webPageName);
    const bool remote =
        path.compare(0, 7, "http://")  == 0 ||
        path.compare(0, 8, "https://") == 0 ||
        path.compare(0, 6, "ftp://")   == 0;

    if (remote) {
        const std::string msg = "[external link] " + path;
        SetWindowText(hwnd, msg.c_str());
        return 0;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        const std::string msg = "[cannot open] " + path;
        SetWindowText(hwnd, msg.c_str());
        return -1;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = htmlToText(ss.str().c_str());
    SetWindowText(hwnd, text.c_str());
    return 0;
}

// Renders an in-memory HTML string. This is the path the scenario browser
// actually uses: TabScenario.cpp passes the BEGIN_HYPERDESC block here.
long WINAPI DisplayHTMLStr(HWND hwnd, const char *string)
{
    if (!hwnd) return -1;
    const std::string text = htmlToText(string);
    SetWindowText(hwnd, text.c_str());
    return 0;
}

} // extern "C"
