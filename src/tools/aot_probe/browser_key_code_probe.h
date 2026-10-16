#ifndef REPIU_TOOLS_AOT_PROBE_BROWSER_KEY_CODE_PROBE_H_
#define REPIU_TOOLS_AOT_PROBE_BROWSER_KEY_CODE_PROBE_H_

namespace repiu::tools
{

// Issue #76. The browser KeyboardEvent.code names the page sends, turned into
// SDL scancodes: the computed ranges, the named keys, the default layout's
// keys, and names that are not keys.
bool RunBrowserKeyCodeProbe();

}  // namespace repiu::tools

#endif  // REPIU_TOOLS_AOT_PROBE_BROWSER_KEY_CODE_PROBE_H_
