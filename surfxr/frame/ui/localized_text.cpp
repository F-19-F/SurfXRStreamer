#include "localized_text.hpp"
#include "surfxr/localization.hpp"
#include <algorithm>
#ifdef SURFXR_FRAME_TEXT
#include <pango/pangocairo.h>
#include <unordered_map>
#include <vector>
#endif

namespace surfxr::ui_detail {
#ifdef SURFXR_FRAME_TEXT
namespace {
struct Mask {unsigned width,height,stride;std::vector<unsigned char> pixels;};
const Mask& glyph_mask(const std::string& text,unsigned scale) {
    static thread_local std::unordered_map<std::string,Mask> cache;
    const auto key=std::to_string(scale)+":"+text;
    auto found=cache.find(key);
    if(found==cache.end()) {
        auto* surface=cairo_image_surface_create(CAIRO_FORMAT_A8,1,1);
        auto* cr=cairo_create(surface);auto* layout=pango_cairo_create_layout(cr);
        auto* font=pango_font_description_new();
        pango_font_description_set_family(font,"SurfXR UI Sans, sans-serif");
        pango_font_description_set_absolute_size(font,7*scale*PANGO_SCALE);
        pango_layout_set_font_description(layout,font);pango_font_description_free(font);
        pango_layout_set_text(layout,text.c_str(),-1);
        int width=0,height=0;pango_layout_get_pixel_size(layout,&width,&height);
        if(width>4096){
            pango_layout_set_width(layout,4096*PANGO_SCALE);
            pango_layout_set_ellipsize(layout,PANGO_ELLIPSIZE_END);
            pango_layout_get_pixel_size(layout,&width,&height);
        }
        PangoRectangle ink{};pango_layout_get_pixel_extents(layout,&ink,nullptr);
        width=ink.width;height=ink.height;
        cairo_destroy(cr);cairo_surface_destroy(surface);
        surface=cairo_image_surface_create(CAIRO_FORMAT_A8,std::max(1,width),std::max(1,height));
        cr=cairo_create(surface);cairo_set_source_rgba(cr,1,1,1,1);
        cairo_move_to(cr,-ink.x,-ink.y);
        pango_cairo_show_layout(cr,layout);cairo_surface_flush(surface);
        Mask mask{static_cast<unsigned>(std::max(1,width)),static_cast<unsigned>(std::max(1,height)),
            static_cast<unsigned>(cairo_image_surface_get_stride(surface)),{}};
        const auto* data=cairo_image_surface_get_data(surface);
        mask.pixels.assign(data,data+mask.stride*mask.height);
        g_object_unref(layout);cairo_destroy(cr);cairo_surface_destroy(surface);
        if(cache.size()>=256)cache.clear();
        found=cache.emplace(key,std::move(mask)).first;
    }
    return found->second;
}
}
#endif
bool localized_size(const std::string& text,unsigned scale,UiTextSize& size) {
#ifdef SURFXR_FRAME_TEXT
    if(std::all_of(text.begin(),text.end(),[](unsigned char c){return c<128;})||!i18n::initialize_fonts())return false;
    const auto& mask=glyph_mask(text,scale);size={mask.width,mask.height};return true;
#else
    (void)text;(void)scale;(void)size;return false;
#endif
}
bool localized_text(std::uint8_t* rgba,unsigned w,unsigned h,unsigned stride,
    unsigned x,unsigned y,const std::string& text,unsigned scale,std::array<std::uint8_t,4> color) {
#ifdef SURFXR_FRAME_TEXT
    UiTextSize size;if(!localized_size(text,scale,size))return false;
    if(!rgba||stride<std::uint64_t{w}*4||x>=w||y>=h)return true;
    const auto& mask=glyph_mask(text,scale);
    for(unsigned row=0;row<std::min(mask.height,h-y);++row)
        for(unsigned col=0;col<std::min(mask.width,w-x);++col) {
            const unsigned alpha=unsigned(mask.pixels[row*mask.stride+col])*color[3]/255;
            if(!alpha)continue;
            auto* pixel=rgba+std::size_t(y+row)*stride+std::size_t(x+col)*4;
            const unsigned output=alpha+unsigned(pixel[3])*(255-alpha)/255;
            for(unsigned c=0;c<3;++c)
                pixel[c]=static_cast<std::uint8_t>((unsigned(color[c])*alpha+unsigned(pixel[c])*pixel[3]*(255-alpha)/255)/output);
            pixel[3]=static_cast<std::uint8_t>(output);
        }
    return true;
#else
    (void)rgba;(void)w;(void)h;(void)stride;(void)x;(void)y;(void)text;(void)scale;(void)color;
    return false;
#endif
}
}
