#include "gb.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static gb_frame parse(const char *s)
{gb_frame f;gb_frame_line(s,(int)strlen(s),&f);return f;}

int main(void)
{
    gb_frame f=parse("\x10setTime(1792497600);E.setTimeZone(5.5);");
    assert(f.has_time && f.epoch==1792497600 && f.has_tz && f.tz_hours==5.5f);
    const char *notification="GB({\"t\":\"notify\",\"body\":\"Example: setTime(1792497600); E.setTimeZone(5.5)\"});";
    f=parse(notification);assert(f.json && !f.has_time && !f.has_tz);
    f=parse("{\"t\":\"notify\",\"body\":\"GB({}); setTime(1792497600)\"}");
    assert(f.json && f.json[0]=='{' && !f.has_time && !f.has_tz);
    f=parse("GB({\"t\":\"notify\",\"body\":\"escaped \\\"setTime(1792497600)\\\"\"});setTime(1792497601);");
    assert(f.json && f.has_time && f.epoch==1792497601 && !f.has_tz);
    const char *ignored[]={"'setTime(1792497600)'", "`setTime(1792497600)`", "// setTime(1792497600)",
        "/* setTime(1792497600) */", "foo(setTime(1792497600))", "{setTime(1792497600)}",
        "notsetTime(1792497600)", "Documentation setTime(1792497600)", "setTime(1792497600oops)",
        "setTime(1e999)", "setTime(9223372036854775808)", "setTime(-1)", "setTimeZone(15)",
        "setTimeZone(5.5 garbage)", "setTime(1792497600"};
    for(unsigned i=0;i<sizeof ignored/sizeof ignored[0];i++) {
        f=parse(ignored[i]);assert(!f.has_time && !f.has_tz);
    }
    f=parse("/* example setTime(1) */ E.setTimeZone( -3.5 );");
    assert(!f.has_time && f.has_tz && f.tz_hours==-3.5f);
    puts("GB framing: top-level commands, quoted notification data and numeric bounds passed");
    return 0;
}
