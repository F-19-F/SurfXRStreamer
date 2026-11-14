#include "surfxr/localization.hpp"
#ifdef SURFXR_FRAME_TEXT
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <sys/mman.h>
#include <unistd.h>
#include "font_data.hpp"
#endif

namespace surfxr::i18n {
bool initialize_fonts() {
#ifdef SURFXR_FRAME_TEXT
    // Embed the font without changing installed assets or OS font settings.
    static const bool ready=[] {
        const int fd=memfd_create("surfxr-ui-font",MFD_CLOEXEC);
        if(fd<0)return false;
        std::size_t offset=0;
        while(offset<sizeof(font_data)) {
            const auto count=write(fd,font_data+offset,sizeof(font_data)-offset);
            if(count<=0){close(fd);return false;}
            offset+=static_cast<std::size_t>(count);
        }
        const auto path="/proc/self/fd/"+std::to_string(fd);
        if(!FcConfigAppFontAddFile(FcConfigGetCurrent(),reinterpret_cast<const FcChar8*>(path.c_str()))) {
            close(fd);return false;
        }
        // Fontconfig reopens this descriptor during glyph lookup. Its lifetime
        // is the process lifetime; the OS closes it on exit.
        pango_font_map_changed(pango_cairo_font_map_get_default());
        return true;
    }();
    return ready;
#else
    return false;
#endif
}
}
