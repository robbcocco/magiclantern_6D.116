/* Astrophotography Helper Module */
#include <module.h>
#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <config.h>
#include <menu.h>
#include "raw.h"
#include "zebra.h"
#include <math.h>
#include "lens.h"
#include "shoot.h"
#include "lvinfo.h"
#include "beep.h"


// TODO should check raw spotmeter supporting  #ifdef RAW_  could compile on 100D & 1100D
// #ifdef CONFIG_RAW_LIVEVIEW ez kell
// Does the Beep function exist on all cameras...?
// http://nanomad.magiclantern.fm/jenkins/features.html

#define STARFOCUS_SIZE 30
extern int bmp_color_scheme;
// Star focus
static CONFIG_INT("starfocus.enabled", starfocus_enabled, 0);
static CONFIG_INT("starfocus.mode", starfocus_mode, 0);
static CONFIG_INT("starfocus.starlevel", starfocus_starlevel, 15); // percentage
static CONFIG_INT("starfocus.bgrlevel", starfocus_bgrlevel, 60); // percentage
static CONFIG_INT("starfocus.size", starfocus_size, STARFOCUS_SIZE);

// Polar alignment
static CONFIG_INT("polalign.enabled", polal_enabled, 0);
static CONFIG_INT("polalign.hemisphere", polal_hemisphere, 0);
static CONFIG_INT("polalign.tutorial", polal_tutorial, 1);

// Color scheme
static CONFIG_INT("color.scheme", color_scheme, 0);

// starfocus task
static int starfocus_task_created = 0;
static int starfocus_task_stop = 0;

// polalign task
static int polalign_task_created = 0;
static int polalign_task_stop = 0;


typedef struct
{
    int x;
    int y;
    int hfd;
    int maxpixel;
} starinfo;

static starinfo start_info;
static starinfo turn_info;
static starinfo end_info;
starinfo bg_info;

inline float Lerp(float value, int inStart, int inEnd, int outStart, int outEnd)
{
    if (value > inEnd) value = inEnd;
    float normed = (value - inStart) / (inEnd - inStart);
    return ( outStart + (normed * (outEnd - outStart)));
}

/*##############################################################################
#	
#   Star Focus using the Half Flux Diameter formula
#
################################################################################*/

static int starfocus_dirty = 0;

static int fast_buffer[(STARFOCUS_SIZE)*2 + 2][(STARFOCUS_SIZE)*2 + 2];

static void starfocus_adjust_star_level(void* priv, int delta)
{
    starfocus_starlevel = (int) starfocus_starlevel + 1 * delta;
    if ((int) starfocus_starlevel > 10) starfocus_starlevel = 1;
    if ((int) starfocus_starlevel <= 0) starfocus_starlevel = 15;
}

static void starfocus_adjust_bgr_level(void* priv, int delta)
{
    starfocus_bgrlevel = (int) starfocus_bgrlevel + 1 * delta;
    if ((int) starfocus_bgrlevel > 70) starfocus_bgrlevel = 1;
    if ((int) starfocus_bgrlevel <= 0) starfocus_bgrlevel = 70;
}

void starfocus_erase_rect(int x1, int y1, int x2, int y2)
{
    uint32_t* M = (uint32_t*) get_bvram_mirror();
    uint32_t* B = (uint32_t*) bmp_vram();

    for (int y = (y1&~1); y <= (y2&~1); y++)
    {
        for (int x = x1; x <= x2; x += 4)
        {
            uint8_t* m = (uint8_t*) (&(M[BM(x, y) / 4])); //32bit to 8bit 
            if (*m == 0x80) *m = 0;
            m++;
            if (*m == 0x80) *m = 0;
            m++;
            if (*m == 0x80) *m = 0;
            m++;
            if (*m == 0x80) *m = 0;
            B[BM(x, y) / 4] = 0;
        }
    }
}

void starfocus_erase(int x, int y, int force_erase)
{
    if (!starfocus_dirty && !force_erase)return;
    starfocus_dirty = 0;
    int xoffset = starfocus_mode > 0 ? 40 : 0;
    starfocus_erase_rect(460 + xoffset+x, 194 - font_small.height+y, 520 + xoffset+x, 193+y);
    starfocus_erase_rect(500+x, 223 - font_small.height+y, 560+x, 222+y);
}

static int spotmeter_offset_x = 0;
static int spotmeter_offset_y = 0;
static int last_dispsize = 1;
static int raw_flag = 0;

static void starfocus_task()
{
    int cleaned = 0;
    TASK_LOOP{

        starfocus_size = starfocus_mode > 0 ? 20 : STARFOCUS_SIZE;

        update_lens_display(0, 0); 

        if (starfocus_task_stop) break;
        if (!starfocus_enabled) break;

        if (!lv) goto starfocus_loop_end;
        if (gui_menu_shown()) goto starfocus_loop_end;

        if (lv_dispsize == 10 || !get_global_draw() ||
                digic_zoom_overlay_enabled() || !lv_luma_is_accurate())
        {
            if (!cleaned)
            {
                cleaned = 1;
                clrscr();
            }
            if (lv_dispsize == 10 && raw_flag) 
            {
                raw_lv_release();
                msleep(100);
                raw_flag = 0;
            }
            if (lv_dispsize == 10)
            {
                int fnt_title = FONT(SHADOW_FONT(FONT_LARGE), COLOR_WHITE, COLOR_BLACK);

                bmp_printf(
                           fnt_title | FONT_ALIGN_CENTER,
                           720 / 2, 480 / 2 - 50,
                           "10x Zoom mode is not supported!"
                           );
                last_dispsize = 10;
            }

            goto starfocus_loop_end;
        }
        if (last_dispsize == 10)
        {
            cleaned = 1;
            clrscr();
            last_dispsize = lv_dispsize;
        }

        cleaned = 0;

        struct vram_info * vram = get_yuv422_vram();

        if (!vram->vram)
            return;

        const uint16_t* vr = (uint16_t*) vram->vram;
        const unsigned width = vram->width;
        int x, y;

        int xcb = os.x0 + os.x_ex / 2;
        int ycb = os.y0 + os.y_ex / 2;

         if (1) // AF frame
         {
             int aff_x0 = 360;
             int aff_y0 = 240;
             if (lv)
             {
                 if (lv_dispsize == 1)
                     get_afframe_pos(720, 480, &aff_x0, &aff_y0);
             }
             else
             {
                 spotmeter_offset_x = COERCE(spotmeter_offset_x, -300, 300);
                 spotmeter_offset_y = COERCE(spotmeter_offset_y, -200, 200);
                 aff_x0 = 360 + spotmeter_offset_x;
                 aff_y0 = 240 + spotmeter_offset_y;
             }
             xcb = N2BM_X(aff_x0);
             ycb = N2BM_Y(aff_y0);
             xcb = COERCE(xcb, os.x0 + 50, os.x_max - 50);
             ycb = COERCE(ycb, os.y0 + 50, os.y_max - 50);
         }
       
        starfocus_dirty = 1;

        int xcl = BM2LV_X(xcb);
        int ycl = BM2LV_Y(ycb);
        int dxl = BM2LV_DX(starfocus_size);

        // Half flux diameter
        int col = 0;
        int brightest = -10000; // brightest luminance
        static unsigned long long HFD = 0; 
        static int averaged = 0;

        int fastbuf_x = 0;
        int fastbuf_y = 0;

        if (starfocus_mode == 0)
        {
            uint8_t * const bvram = bmp_vram();
            if (!bvram) return;

            // find the brightest pixel
            fastbuf_x = 0;
            fastbuf_y = 0;

            for (y = ycl - dxl; y <= ycl + dxl; y++)
            {
                fastbuf_y++;
                for (x = xcl - dxl; x <= xcl + dxl; x++)
                {
                    fastbuf_x++;
                    uint16_t p = vr[ x + y * width ];
                    col = (p >> 8) & 0xFF; 
                    if (col > brightest)
                    {
                        brightest = col;
                    }
                    fast_buffer[fastbuf_x][fastbuf_y] = col; 
                }
                fastbuf_x = 0;
            }

          
            int threshold = 255 * starfocus_starlevel / 100;
            int centroid_x = 0;
            int centroid_y = 0;
            int above_threshold_count = 0;
			
            // find the centroid of bright pixels
            fastbuf_x = 0;
            fastbuf_y = 0;
            for (y = ycl - dxl; y <= ycl + dxl; y++)
            {
                fastbuf_y++;
                for (x = xcl - dxl; x <= xcl + dxl; x++)
                {
                    fastbuf_x++;
                    col = fast_buffer[fastbuf_x][fastbuf_y]; 
                    if (col >= brightest - threshold)
                    {
                        centroid_x += x;
                        centroid_y += y;
                        above_threshold_count++;
                    }
                }
                fastbuf_x = 0;
            }

            centroid_x /= above_threshold_count;
            centroid_y /= above_threshold_count;

            
            int distance_from_centroid = 0;
            int noise_thres = brightest * starfocus_bgrlevel / 100;
            unsigned sum_luminance = 0;
            unsigned sum_luminance_distance = 0;
            fastbuf_x = 0;
            fastbuf_y = 0;
            for (y = ycl - dxl; y <= ycl + dxl; y++)
            {
                fastbuf_y++;
                for (x = xcl - dxl; x <= xcl + dxl; x++)
                {
                    fastbuf_x++;
                    col = fast_buffer[fastbuf_x][fastbuf_y]; // reuse                 
                    if (col > noise_thres)
                    {
                        distance_from_centroid = sqrtf((centroid_x - x)*(centroid_x - x) + (centroid_y - y)*(centroid_y - y));
                        sum_luminance += col;
                        sum_luminance_distance += col * distance_from_centroid;
                    }
                }
                fastbuf_x = 0;
            }
           
            if (averaged == 10)
            {
                HFD = 0;
                averaged = 0;
            }
            if (averaged < 10)
            {
                averaged++;
                HFD += (sum_luminance_distance * 100) / sum_luminance;
            }
        }

        if (starfocus_mode == 1)
        {
            if (!raw_flag) // RAW HFD
            {
                raw_lv_request(); 
                raw_flag = 1;
            }

            if (can_use_raw_overlays() && raw_update_params())
            {
                const int xcr = BM2RAW_X(xcb);
                const int ycr = BM2RAW_Y(ycb);
                const int dxr = BM2RAW_DX(starfocus_size);

                // find the brightest pixel
                for (y = ycr - dxr; y <= ycr + dxr; y++)
                {
                    if (y < raw_info.active_area.y1 || y > raw_info.active_area.y2) continue;
                    for (x = xcr - dxr; x <= xcr + dxr; x++)
                    {
                        if (x < raw_info.active_area.x1 || x > raw_info.active_area.x2) continue;
                        col = raw_get_pixel(x, y) - raw_info.black_level;
                        if (col > brightest)
                        {
                            brightest = col;
                        }
                    }
                }

                int threshold = 14366 * starfocus_starlevel / 100;
                int centroid_x = 0;
                int centroid_y = 0;
                int above_thresold_count = 0;

                for (y = ycr - dxr; y <= ycr + dxr; y++)
                {
                    if (y < raw_info.active_area.y1 || y > raw_info.active_area.y2) continue;

                    for (x = xcr - dxr; x <= xcr + dxr; x++)
                    {
                        if (x < raw_info.active_area.x1 || x > raw_info.active_area.x2) continue;

                        col = raw_get_pixel(x, y) - raw_info.black_level;
                        if (col >= brightest - threshold)
                        {
                            centroid_x += x;
                            centroid_y += y;
                            above_thresold_count++;
                        }
                    }

                }
                centroid_x /= above_thresold_count;
                centroid_y /= above_thresold_count;

                unsigned distance_from_centroid = 0;
                int noise_thres = brightest * starfocus_bgrlevel / 100;
                unsigned long long sum_luminance = 0;
                unsigned long long sum_luminance_distance = 0;

                for (y = ycr - dxr; y <= ycr + dxr; y++)
                {
                    if (y < raw_info.active_area.y1 || y > raw_info.active_area.y2) continue;
                    for (x = xcr - dxr; x <= xcr + dxr; x++)
                    {
                        if (x < raw_info.active_area.x1 || x > raw_info.active_area.x2) continue;
                        col = raw_get_pixel(x, y) - raw_info.black_level;
                        if (col > noise_thres)
                        {
                            distance_from_centroid = sqrtf((centroid_x - x)*(centroid_x - x) + (centroid_y - y)*(centroid_y - y));
                            sum_luminance += col;
                            sum_luminance_distance += col * distance_from_centroid;
                        }
                    }

                }

                if (averaged == 10)
                {
                    HFD = 0;
                    averaged = 0;
                }
                if (averaged < 10)
                {
                    averaged++;
                    HFD += (sum_luminance_distance * 100) / sum_luminance;
                }
            }
        }

        draw_circle(xcb, ycb, 10, COLOR_WHITE); // inner circle
        draw_circle(xcb, ycb, 40, COLOR_WHITE); // outer circle
        int l = 3;
        draw_line(xcb - l, ycb, xcb + l, ycb,COLOR_WHITE); // crosshair horizontal
        draw_line(xcb, ycb - l, xcb, ycb + l,COLOR_WHITE); // crosshair vertical

        int xoff = -400;
        int yoff = 200;
        starfocus_erase(xoff, yoff, 0);
        int maxPixValue = starfocus_mode > 0 ? 14337 : 255; 
        int maxHFDValue = starfocus_mode > 0 ? (lv_dispsize > 1) ? 2000 : 1200 : (lv_dispsize > 1) ? 1400 : 800;
        int br_lerp = Lerp((brightest), 0, maxPixValue, 0, 284);
        int hfd_lerp = Lerp((HFD / averaged), 1, maxHFDValue, 1, 284);
        int fnt_mini = FONT(SHADOW_FONT(FONT_SMALL), COLOR_WHITE, COLOR_GRAY(10));

        bmp_fill(COLOR_GRAY(60), 420+xoff, 194+yoff, 287, 16);
        bmp_fill(COLOR_BG_DARK, 421+xoff, 195+yoff, 285, 14);


        for (int i = 436; i <= 705; i += 10)
            draw_line(i+xoff, 196+yoff, i+xoff, 207+yoff, COLOR_GRAY(60)); 
        bmp_fill(COLOR_WHITE, 422+xoff, 196+yoff, hfd_lerp, 12); // mozgó bar


        bmp_fill(COLOR_GRAY(60), 420+xoff, 223+yoff, 287, 16);
        bmp_fill(COLOR_BG_DARK, 421+xoff, 224+yoff, 285, 14);
        for (int i = 436; i <= 705; i += 10)
            draw_line(i+xoff, 225+yoff, i+xoff, 236+yoff, COLOR_GRAY(60)); 
        bmp_fill(COLOR_WHITE, 422+xoff, 225+yoff, br_lerp, 12); // mozgó bar

        bmp_printf(
                   fnt_mini,
                   420+xoff, 194 - font_small.height+yoff,
                   "%s %d.%d",
                   starfocus_mode > 0 ? "HFD (RAW):" : "HFD:",
                   (int) HFD / 100 / averaged, (int) (HFD / averaged) % 100
                   );
        bmp_printf(
                   fnt_mini,
                   420+xoff, 223 - font_small.height+yoff,
                   "Max pixel: %d",
                   (int) brightest
                   );

        xcb += 4;

        ycb += 60;
        int xcb2 = xcb;
        starfocus_mode > 0 ? (xcb2 += 175) : (xcb2 += 64); // HFD (RAW):  | HFD: 


        msleep(100); //80
        continue;
starfocus_loop_end:
        msleep(300);

    }
}





/*##############################################################################
#	
#	 Drift Polar Alignment
#
################################################################################*/

static int polal_dirty = 0;

#define PA_STEP_CAMERA_ALIGN_INFO 0
#define PA_STEP_CAMERA_ALIGN 1
#define PA_STEP_ALIGN_AZ_INFO 2
#define PA_STEP_ALIGN_AZ 3
#define PA_STEP_ALIGN_AL_INFO 4
#define PA_STEP_ALIGN_AL 5

#define PA_MAX_STEPS 5

static int polal_curr_step = 0;


#define PA_D_WAIT_START 0
#define PA_D_WAIT_TURN 1
#define PA_D_WAIT_END 2
#define PA_D_SHOW_RESULT 3

#define PA_D_MAX_STEPS 3

//#define DRAW_DEBUG 

int barsize = 30; 
static int current_da_step = 0;



int get_hfd_for_da(int px, int py, int pr, starinfo * info)
{
    struct vram_info * vram = get_yuv422_vram();

    if (!vram->vram)
        return 0;

    if (polal_dirty)
    {

        clrscr();
        polal_dirty = 0;
    }

    const uint16_t* vr = (uint16_t*) vram->vram;
    const unsigned width = vram->width;
    int x, y;

    int xcl = BM2LV_X(px);
    int ycl = BM2LV_Y(py);
    int dxl = BM2LV_DX(pr);

    // Half flux diameter
    int col = 0;
    int brightest = -10000; // brightest luminance

    uint8_t * const bvram = bmp_vram();
    if (!bvram) return 0;

    // find the brightest pixel
    for (y = ycl - dxl; y <= ycl + dxl; y++)
    {
        for (x = xcl - dxl; x <= xcl + dxl; x++)
        {
            uint16_t p = vr[ x + y * width ];
            col = (p >> 8) & 0xFF; // 0-255
            if (col > brightest)
            {
                brightest = col;
            }
        }
    }
    int threshold = 20;
    int centroid_x = 0;
    int centroid_y = 0;
    int above_threshold_count = 0;
    // find the centroid of bright pixels
    for (y = ycl - dxl; y <= ycl + dxl; y++)
    {
        for (x = xcl - dxl; x <= xcl + dxl; x++)
        {
            uint16_t p = vr[ x + y * width ];
            col = (p >> 8) & 0xFF; 
            int temp = brightest - threshold;
            if (temp <= 0) temp = 1;
            if (col >= temp)
            {
                centroid_x += x;
                centroid_y += y;
                above_threshold_count++;
            }
        }
    }
    if (above_threshold_count == 0)
    {
        info->hfd = 1099;
        info->maxpixel = brightest;
        info->x = 0;
        info->y = 0;

        return 0;
    }
    centroid_x /= above_threshold_count;
    centroid_y /= above_threshold_count;

    // Find the Half Flux Diameter
    int distance_from_centroid = 0;
    int noise_thres = brightest * 60 / 100; 
    unsigned sum_luminance = 0;
    unsigned sum_luminance_distance = 0;
    for (y = ycl - dxl; y <= ycl + dxl; y++)
    {
        for (x = xcl - dxl; x <= xcl + dxl; x++)
        {
            uint16_t p = vr[ x + y * width ];

            col = (p >> 8) & 0xFF; 

            if (col > noise_thres)
            {
                distance_from_centroid = sqrtf((centroid_x - x)*(centroid_x - x) + (centroid_y - y)*(centroid_y - y));
                sum_luminance += col;
                sum_luminance_distance += col * distance_from_centroid;
            }
        }
    }

    info->hfd = (sum_luminance_distance * 100) / sum_luminance;
    info->maxpixel = brightest;
    info->x = centroid_x;
    info->y = centroid_y;

    return 1;
}
void get_sky_background_info(starinfo * bg)
{
    starinfo bg_info1, bg_info2, bg_info3, bg_info4; // holding sky background data

    get_hfd_for_da(720 / 3, 480 / 3, 10, &bg_info1); // upper left
    get_hfd_for_da(720 / 3 * 2, 480 / 3, 10, &bg_info2); // upper right
    get_hfd_for_da(720 / 3, 480 / 3 * 2, 10, &bg_info3); // bottom left
    get_hfd_for_da(720 / 3 * 2, 480 / 3 * 2, 10, &bg_info4); // bottom right

#ifdef DRAW_DEBUG

    draw_circle(720 / 3, 480 / 3, 10, COLOR_BG_DARK);
    draw_circle(720 / 3 * 2, 480 / 3, 10, COLOR_BG_DARK);
    draw_circle(720 / 3, 480 / 3 * 2, 10, COLOR_BG_DARK);
    draw_circle(720 / 3 * 2, 480 / 3 * 2, 10, COLOR_BG_DARK);
#endif


    bg->hfd = (bg_info1.hfd + bg_info2.hfd + bg_info3.hfd + bg_info4.hfd) / 4;
    bg->maxpixel = (bg_info1.maxpixel + bg_info2.maxpixel + bg_info3.maxpixel + bg_info4.maxpixel) / 4;

}
int is_starinfo_similar(starinfo * a, starinfo * b, int thres)
{
    int hfd_same = 0;
    for (int diff = 0; diff < thres; diff++) 
    {
        if (a->hfd + diff == b->hfd) hfd_same = 1;
        if (a->hfd - diff == b->hfd) hfd_same = 1;
    }

    if (hfd_same)
    {
        for (int diff = 0; diff < thres; diff++) // for stars it is usually ~ 255 
        {
            if (a->maxpixel + diff == b->maxpixel) return 1;
            if (a->maxpixel - diff == b->maxpixel) return 1;
        }
    }

    return 0;
}


static int n_times_less_then_bgr = 0;
void reset_drift()
{
    current_da_step = 0;
}

void compute_drift()
{
    int dcr = 10; // detection circle radius
    int fnt_title = FONT(SHADOW_FONT(FONT_LARGE), COLOR_WHITE, COLOR_GRAY(10));
    int fnt = FONT(SHADOW_FONT(FONT_MED), COLOR_WHITE, COLOR_GRAY(10));

    switch (current_da_step)
    {
    case PA_D_WAIT_START:
        reset_drift();



        bmp_printf(
                   fnt | FONT_ALIGN_CENTER,
                   720 / 2, font_large.height + 8 + barsize,
                   "Slew your mount until the star gets inside the circle."
                   );



        draw_line(720, 480 / 2, 680, 480 / 2, COLOR_WHITE); // right line
        draw_circle(680 - dcr, 480 / 2, dcr, COLOR_WHITE); // right circle
        draw_line(0, 480 / 2, 680 - dcr * 2, 480 / 2, COLOR_WHITE); //left line


        if (!get_hfd_for_da(680 - 10, 480 / 2, 10, &start_info)) break;




        get_sky_background_info(&bg_info);


        // TODO: átlagolás kellene
        if (start_info.hfd < bg_info.hfd / 2)
        {
            n_times_less_then_bgr++;
            if (n_times_less_then_bgr > 7)
            {
                get_hfd_for_da(680 - 10, 480 / 2, 10, &start_info/*, 20*/);
                n_times_less_then_bgr = 0;
                polal_dirty = 1;
                current_da_step++;
                beep_custom(50, 500, 0);
            }
        }
        else n_times_less_then_bgr = 0;

#ifdef DRAW_DEBUG
        erase_rect(720 / 2 - 100, 150, 720 / 2 + 100, 210);
        bmp_printf(
                   fnt | FONT_ALIGN_CENTER,
                   720 / 2, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   bg_info.x, bg_info.y, bg_info.hfd / 100, bg_info.hfd % 100, bg_info.maxpixel
                   );




        erase_rect(490, 150, 720, 210);
        bmp_printf(
                   fnt,
                   500, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   start_info.x, start_info.y, start_info.hfd / 100, start_info.hfd % 100, start_info.maxpixel
                   );
#endif
        break;
    case PA_D_WAIT_TURN:
#ifdef DRAW_DEBUG

        erase_rect(490, 150, 720, 210);
        bmp_printf(
                   fnt,
                   500, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   start_info.x, start_info.y, start_info.hfd / 100, start_info.hfd % 100, start_info.maxpixel
                   );
#endif

        bmp_printf(
                   fnt | FONT_ALIGN_CENTER,
                   720 / 2, font_large.height + 8 + barsize,
                   "Slew your mount with your GoTo controller so that the star is\n"
                   "inside the circle on the left."
                   );

        draw_line(720, 480 / 2, 680, 480 / 2, COLOR_WHITE); // right line
        draw_circle(680 - dcr, 480 / 2, dcr, COLOR_GREEN2); // right GREEN circle

        draw_line(50 + dcr, 480 / 2, 680 - dcr * 2, 480 / 2, COLOR_WHITE); // middle line
        draw_circle(60 - dcr, 480 / 2, dcr, COLOR_WHITE); // left RED circle
        draw_line(0, 480 / 2, 40, 480 / 2, COLOR_WHITE); // left line


        if (!get_hfd_for_da(60 - 10, 480 / 2, 10, &turn_info))break;

        if (is_starinfo_similar(&start_info, &turn_info, 30))
        {

            n_times_less_then_bgr++;
            if (n_times_less_then_bgr > 2)
            {
                get_hfd_for_da(60 - 10, 480 / 2, 10, &turn_info);
                polal_dirty = 1;
                current_da_step++;
                beep_custom(50, 500, 1);
                beep_custom(50, 500, 0);
            }
        }
        else n_times_less_then_bgr = 0;

#ifdef DRAW_DEBUG

        erase_rect(20, 150, 250, 210);
        bmp_printf(
                   fnt,
                   20, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   n_times_less_then_bgr, turn_info.y, turn_info.hfd / 100, turn_info.hfd % 100, turn_info.maxpixel
                   );
#endif


        break;
    case PA_D_WAIT_END:

#ifdef DRAW_DEBUG

        erase_rect(490, 150, 720, 210);
        bmp_printf(
                   fnt,
                   500, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   start_info.x, start_info.y, start_info.hfd / 100, start_info.hfd % 100, start_info.maxpixel
                   );



        erase_rect(20, 150, 250, 210);
        bmp_printf(
                   fnt,
                   20, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   turn_info.x, turn_info.y, turn_info.hfd / 100, turn_info.hfd % 100, turn_info.maxpixel
                   );

#endif




        for (int i = 10; i < 460; i += 20)
        {
            end_info.hfd = 0;
            end_info.maxpixel = 0;
            if (!get_hfd_for_da(680 - 10, i, 10, &end_info)) break;
#ifdef DRAW_DEBUG 
            draw_circle(680 - 10, i, dcr, COLOR_BG_DARK); // right circle
#endif
            if (is_starinfo_similar(&start_info, &end_info, 30))
            {
                n_times_less_then_bgr++;
                if (n_times_less_then_bgr > 3)
                {
                    get_hfd_for_da(680 - 10, i, 10, &end_info);
                    polal_dirty = 1;
                    current_da_step++;
                    beep_custom(50, 500, 1); // 1st beep
                    beep_custom(50, 500, 1); // 2nd beep
                    msleep(100); // to hear the 3rd beep too
                    beep_custom(50, 500, 0); // 3rd beep
                    n_times_less_then_bgr = 0;
                    break;
                }

            }
        }








#ifdef DRAW_DEBUG
        erase_rect(720 / 2 - 100, 150, 720 / 2 + 130, 210);
        bmp_printf(
                   fnt | FONT_ALIGN_CENTER,
                   720 / 2, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   end_info.x, end_info.y, end_info.hfd / 100, end_info.hfd % 100, end_info.maxpixel
                   );
#endif




        bmp_printf(
                   fnt | FONT_ALIGN_CENTER,
                   720 / 2, font_large.height + 8 + barsize,
                   "Now slew backward until the star touches the \n"
                   "dotted vertical line, then stop."
                   );


        fill_circle(start_info.x, start_info.y, 4, COLOR_WHITE);
        fill_circle(turn_info.x, turn_info.y, 4, COLOR_WHITE);
        draw_line(start_info.x, start_info.y, turn_info.x, turn_info.y, COLOR_WHITE);


        for (int y = 0; y < 480; y += 10)
            draw_line(680 - dcr, y, 680 - dcr, y + 5, COLOR_WHITE);


        break;
    case PA_D_SHOW_RESULT:
#ifdef DRAW_DEBUG
        erase_rect(720 / 2 - 100, 150, 720 / 2 + 100, 210);

        bmp_printf(
                   fnt | FONT_ALIGN_CENTER,
                   720 / 2, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   end_info.x, end_info.y, end_info.hfd / 100, end_info.hfd % 100, end_info.maxpixel
                   );

        erase_rect(490, 150, 720, 210);

        bmp_printf(
                   fnt,
                   500, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   start_info.x, start_info.y, start_info.hfd / 100, start_info.hfd % 100, start_info.maxpixel
                   );



        erase_rect(20, 150, 250, 210);

        bmp_printf(
                   fnt,
                   20, 200 - font_med.height * 2 - 5,
                   "x:%d y:%d\nHFD:%d.%d MP:%d",
                   turn_info.x, turn_info.y, turn_info.hfd / 100, turn_info.hfd % 100, turn_info.maxpixel
                   );

#endif

        fill_circle(start_info.x, start_info.y, 4, COLOR_WHITE);
        fill_circle(turn_info.x, turn_info.y, 4, COLOR_WHITE);
        fill_circle(end_info.x, end_info.y, 4, COLOR_WHITE);
        draw_line(start_info.x, start_info.y, turn_info.x, turn_info.y, COLOR_WHITE);
        draw_line(turn_info.x, turn_info.y, end_info.x, end_info.y, COLOR_WHITE);

      
        int diff = start_info.y - end_info.y; 
        int abs_diff = ABS(start_info.y - end_info.y);


        if (polal_curr_step == PA_STEP_ALIGN_AZ)
        {

            if (abs_diff > 5) // if the difference is big
            {
                if (polal_hemisphere < 1) // northern hemisphere
                {
                    bmp_printf(
                               fnt | FONT_ALIGN_CENTER,
                               720 / 2, 120 - font_med.height * 2,
                               "Difference: %d\nThe telescope is pointing too far %s.\nMake a correction to the azimuth control\nby moving the telescope %s.",
                               abs_diff, diff > 0 ? "West" : "East", diff > 0 ? "East" : "West"
                               );
                }
                else if (polal_hemisphere > 0) // southern hemisphere
                {
                    bmp_printf(
                               fnt | FONT_ALIGN_CENTER,
                               720 / 2, 120 - font_med.height * 2,
                               "Difference: %d\nTelescope is pointing too far %s.\nMake a correction to the azimuth control\nby moving the telescope %s.",
                               abs_diff, diff > 0 ? "East" : "West", diff > 0 ? "West" : "East"
                               );
                }
            }
            else
            {
                if (polal_hemisphere < 1) // northern hemisphere
                {
                    bmp_printf(
                               fnt | FONT_ALIGN_CENTER,
                               720 / 2, 120 - font_med.height * 2,
                               "Difference: %d\nSeems OK!\nBut you can make just a little correction to the azimuth\nby moving the telescope %s.",
                               abs_diff, diff > 0 ? "East" : "West"
                               );
                }
                else if (polal_hemisphere > 0) // southern hemisphere
                {
                    bmp_printf(
                               fnt | FONT_ALIGN_CENTER,
                               720 / 2, 120 - font_med.height * 2,
                               "Difference: %d\nSeems OK!\nBut you can make just a little correction to the azimuth\nby moving the telescope %s.",
                               abs_diff, diff > 0 ? "West" : "East"
                               );
                }
            }

            bmp_printf(
                       fnt_title | FONT_ALIGN_CENTER,
                       720 / 2, 480 - font_med.height * 3 - barsize,
                       "Press SET to align Altitude!"
                       );
        }
        else if (polal_curr_step == PA_STEP_ALIGN_AL)
        {

            if (abs_diff > 5) // if the difference is big
            {
                if (polal_hemisphere < 1) // northern hemisphere
                {
                    bmp_printf(
                               fnt | FONT_ALIGN_CENTER,
                               720 / 2, 120 - font_med.height * 2,
                               "Difference: %d\nThe telescope is pointing too %s.\n%s the altitude a bit, then try again!",
                               abs_diff, diff > 0 ? "high" : "low", diff > 0 ? "Lower" : "Raise"
                               );
                }
                else if (polal_hemisphere > 0) // southern hemisphere
                {
                    bmp_printf(
                               fnt | FONT_ALIGN_CENTER,
                               720 / 2, 120 - font_med.height * 2,
                               "Difference: %d\nThe telescope is pointing too %s.\n%s the altitude a bit, then try again!",
                               abs_diff, diff > 0 ? "low" : "high", diff > 0 ? "Raise" : "Lower"
                               );
                }
            }
            else
            {
                if (polal_hemisphere < 1) // northern hemisphere
                {
                    bmp_printf(
                               fnt | FONT_ALIGN_CENTER,
                               720 / 2, 120 - font_med.height * 2,
                               "Difference: %d\nSeems OK!\nBut you can still %s the altitude a little bit if you want.",
                               abs_diff, diff > 0 ? "raise" : "lower"
                               );
                }
                else if (polal_hemisphere > 0) // southern hemisphere
                {
                    bmp_printf(
                               fnt | FONT_ALIGN_CENTER,
                               720 / 2, 120 - font_med.height * 2,
                               "Difference: %d\nSeems OK!\nBut you can still %s the altitude a little bit if you want.",
                               abs_diff, diff > 0 ? "lower" : "raise"
                               );
                }
            }

            bmp_printf(
                       fnt | FONT_ALIGN_CENTER,
                       720 / 2, 480 - font_med.height * 4 - barsize,
                       "Press Right to recheck azimuth again!"
                       );

            bmp_printf(
                       fnt | FONT_ALIGN_CENTER,
                       720 / 2, 480 - font_med.height * 3 - barsize,
                       "Press SET to Quit!"
                       );
        }

        break;
    }
}

void draw_cam_align_info(void)
{

    int fnt_title = FONT(SHADOW_FONT(FONT_LARGE), COLOR_WHITE, COLOR_GRAY(10));

    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 5 + barsize,
               "Camera Alignment"
               );

    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 480 / 2 - font_med.height * 2,
               "First align your camera, so that the stars\nmove horizontally across the\ncamera's sensor.\n\nPress SET to start!"
               );
}

void draw_cam_align()
{
    int fnt_title = FONT(SHADOW_FONT(FONT_LARGE), COLOR_WHITE, COLOR_GRAY(10));
    int fnt = FONT(SHADOW_FONT(FONT_MED), COLOR_WHITE, COLOR_GRAY(10));

    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 5 + barsize,
               "Aligning camera"
               );

    bmp_printf(
               fnt | FONT_ALIGN_CENTER,
               720 / 2, font_large.height + 8 + barsize,
               "Rotate your camera, until the stars move horizontally across\nthe camera's sensor."
               );

    draw_line(0, 480 / 2, 720, 480 / 2, COLOR_WHITE); // horizontal line

    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 480 - font_med.height * 3 - barsize,
               "Press SET if you are done!"
               );
}

void draw_pol_align_az_info()
{
    int fnt_title = FONT(SHADOW_FONT(FONT_LARGE), COLOR_WHITE, COLOR_GRAY(10));
    int fnt = FONT(SHADOW_FONT(FONT_MED), COLOR_WHITE, COLOR_GRAY(10));


    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 5 + barsize,
               "Azimuth Polar Alignment"
               );

    bmp_printf(
               fnt_title,
               20, font_large.height + 8,
               "Now:"
               );
 
    bmp_printf(
               fnt,
               20, font_med.height * 2 + 24,
               "1: Point your telescope at a fairly bright star, near the meridian\n"
               "   and towards to %s.",
               polal_hemisphere > 0 ? "North" : "South"
               );
    bmp_printf(
               fnt,
               20, font_med.height * 4 + 27,
               "2: Now slew your mount until the star goes inside the circle\n"
               "   on the left side of the screen (%s side of your FOV)",
               polal_hemisphere > 0 ? "West" : "East"
               );
    bmp_printf(
               fnt,
               20, font_med.height * 6 + 27,
               "   If the circle turns to green, you're ready to slew your\n"
               "   scope towards %s with your GoTo controller.",
               polal_hemisphere > 0 ? "East" : "West"
               );
    bmp_printf(
               fnt,
               20, font_med.height * 8 + 27,
               "   Set your mount's slewing speed to be just likely faster than\n"
               "   it's natural tracking speed (2x,4x,8x...)."
               );
    bmp_printf(
               fnt,
               20, font_med.height * 10 + 27,
               "3: Now slew your mount until the star goes into the circle\n"
               "   on the left side of the screen (%s side of your FOV).",
               polal_hemisphere > 0 ? "East" : "West"
               );
    bmp_printf(
               fnt,
               20, font_med.height * 12 + 27,
               "   When the circle on the left side turns to green, slew your\n"
               "   scope backward towards %s with your GoTo controller until",
               polal_hemisphere > 0 ? "West" : "East"
               );
    bmp_printf(
               fnt,
               20, font_med.height * 14 + 27,
               "   the star goes through the dotted vertical line."
               );

    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 480 - font_med.height * 2 - barsize,
               "Press SET to start!"
               );
}

void draw_pol_align_az()
{
    int fnt_title = FONT(SHADOW_FONT(FONT_LARGE), COLOR_WHITE, COLOR_GRAY(10));
    int fnt = FONT(SHADOW_FONT(FONT_MED), COLOR_WHITE, COLOR_GRAY(10));


    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 5 + barsize,
               "Azimuth Polar Alignment"
               );

    compute_drift();

    bmp_printf(
               fnt,
               10, 480 - font_med.height - 5 - barsize,
               "Hemisphere: %s",
               polal_hemisphere > 0 ? "Southern" : "Northern"
               );

    bmp_printf(
               fnt | FONT_ALIGN_RIGHT,
               720 - 10, 480 - font_med.height - 5 - barsize,
               "Press Right to realign Azimuth"
               );

}

void draw_pol_align_al_info()
{
    int fnt_title = FONT(SHADOW_FONT(FONT_LARGE), COLOR_WHITE, COLOR_GRAY(10));
    int fnt = FONT(SHADOW_FONT(FONT_MED), COLOR_WHITE, COLOR_GRAY(10));

    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 5 + barsize,
               "Altitude Polar Alignment"
               );

    bmp_printf(
               fnt_title,
               20, font_large.height + 8,
               "Now"
               );

    bmp_printf(
               fnt,
               20, font_med.height * 2 + 24,
               "1: Point your telescope at a fairly bright star, near the western\n"
               "   or eastern horizon, and do the same test again,"
               );
    bmp_printf(
               fnt,
               20, font_med.height * 4 + 24,
               "   but this time adjust your mount's altitude adjustment T-bolts."
               );
    bmp_printf(
               fnt,
               20, font_med.height * 5 + 27,
               "2: So slew your mount until the star is inside the circle\n"
               "   on the right side of the screen."
               );
    bmp_printf(
               fnt,
               20, font_med.height * 7 + 27,
               "   If the circle turns to green, you're ready to slew your\n"
               "   scope with your GoTo controller."
               );
    bmp_printf(
               fnt,
               20, font_med.height * 9 + 27,
               "   Set your mount's slewing speed to be just likely faster than\n"
               "   it's natural tracking speed (2x,4x,8x...)."
               );
    bmp_printf(
               fnt,
               20, font_med.height * 11 + 27,
               "3: Now slew your mount until the star goes into the circle\n"
               "   on the left side of the screen."
               );
    bmp_printf(
               fnt,
               20, font_med.height * 13 + 27,
               "   When the circle on the left side turns to green, slew your\n"
               "   scope to the opposite direction with your"
               );
    bmp_printf(
               fnt,
               20, font_med.height * 15 + 27,
               "   GoTo controller, until the star touches or goes through the\n"
               "   dotted vertical line."
               );

    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 480 - font_med.height * 2 - barsize + 10,
               "Press SET to start!"
               );
}

void draw_pol_align_al()
{
    int fnt_title = FONT(SHADOW_FONT(FONT_LARGE), COLOR_WHITE, COLOR_GRAY(10));
    int fnt = FONT(SHADOW_FONT(FONT_MED), COLOR_WHITE, COLOR_GRAY(10));

    bmp_printf(
               fnt_title | FONT_ALIGN_CENTER,
               720 / 2, 5 + barsize,
               "Altitude Polar Alignment"
               );

    compute_drift();

    bmp_printf(
               fnt,
               10, 480 - font_med.height - 5 - barsize,
               "Hemisphere: %s",
               polal_hemisphere > 0 ? "Southern" : "Northern"
               );

    bmp_printf(
               fnt | FONT_ALIGN_RIGHT,
               720 - 10, 480 - font_med.height * 1 - 5 - barsize,
               "Press Right to realign Altitude"
               );
}

static void polalign_task()
{
    TASK_LOOP{

        if (polalign_task_stop) break;
        if (!polal_enabled) break;
        if (!lv) goto polal_loop_end;
        if (gui_menu_shown()) goto polal_loop_end;



        if (polal_dirty)
        {

            clrscr();
            polal_dirty = 0;
        }
   
        switch (polal_curr_step)
        {
        case PA_STEP_CAMERA_ALIGN_INFO:
            if (!polal_tutorial)
            {
                polal_curr_step++;
                break;
            }
            draw_cam_align_info();
            break;
        case PA_STEP_CAMERA_ALIGN:
            draw_cam_align();
            break;
        case PA_STEP_ALIGN_AZ_INFO:
            if (!polal_tutorial)
            {
                polal_curr_step++;
                break;
            }
            draw_pol_align_az_info();
            break;
        case PA_STEP_ALIGN_AZ:
            draw_pol_align_az();
            break;
        case PA_STEP_ALIGN_AL_INFO:
            if (!polal_tutorial)
            {
                polal_curr_step++;
                break;
            }
            draw_pol_align_al_info();
            break;
        case PA_STEP_ALIGN_AL:
            draw_pol_align_al();
            break;
        }

#ifdef DRAW_DEBUG

        int fnt = FONT(SHADOW_FONT(FONT_MED), COLOR_WHITE, COLOR_BLACK);
        bmp_printf(
                   fnt,
                   10, 480 - font_med.height * 2 - 10,
                   "Current steps are: %d\t%d",
                   polal_curr_step, current_da_step
                   );
#endif

        msleep(80);
        continue;
        polal_loop_end:
        msleep(300);
    }
}





int astro_keypress_cbr(int ctx)
{
    if (!lv) return 1;
    if (gui_menu_shown()) return 1;
    if (!get_global_draw()) return 1;
    if (polal_enabled)
    {
        if (ctx == MODULE_KEY_PRESS_RIGHT)
        {
            polal_curr_step = PA_STEP_ALIGN_AZ_INFO;
            polal_dirty = 1;
            current_da_step = 0;

            return 0;
        }

        if (ctx == MODULE_KEY_PRESS_SET)
        {
            if (polal_curr_step != PA_STEP_ALIGN_AZ && polal_curr_step != PA_STEP_ALIGN_AL)
            {
                polal_curr_step++;
                if (polal_curr_step > PA_MAX_STEPS)
                    polal_curr_step = 0;
                polal_dirty = 1;
                return 0;
            }
            else if (current_da_step != PA_D_SHOW_RESULT)
            {

                if (current_da_step < PA_D_SHOW_RESULT)
                {
                    current_da_step++;
                    polal_dirty = 1;
                    return 0;
                }
                return 1;
            }
            else if (current_da_step == PA_D_SHOW_RESULT)
            {
                reset_drift(); 
                polal_curr_step++;
                if (polal_curr_step > PA_MAX_STEPS)
                    polal_curr_step = 0;

                polal_dirty = 1;
                return 0;

            }
            return 1;
            if (ctx == MODULE_KEY_PRESS_LEFT && (polal_curr_step == PA_STEP_ALIGN_AZ || polal_curr_step == PA_STEP_ALIGN_AL))
            {
                polal_dirty = 1;
                current_da_step = 0; 
                return 0;
            }
        }
    }
    return 1; 
}

/* called once every second */
static unsigned int astro_step()
{
    if (starfocus_enabled)
    {
        polal_enabled = 0; // turn off polar alignment
        if (!starfocus_task_created)
        {
            starfocus_task_created = 1;
            task_create("starfocus_task", 0x1c, 0x1000, starfocus_task, (void*) 0);
        }
    }
    else
    {
        starfocus_task_stop = 0;
        starfocus_task_created = 0;

        if (raw_flag)
        {
            raw_lv_release();
            msleep(100);
            raw_flag = 0;
        }
    }
    if (polal_enabled)
    {
        starfocus_enabled = 0; // turn off starfocus
        if (!polalign_task_created)
        {
            polalign_task_created = 1;
            task_create("polalign_task", 0x1c, 0x1000, polalign_task, (void*) 0);
        }
    }
    else
    {
        polalign_task_stop = 0;
        polalign_task_created = 0;
    }

    return 0;
}

static int astro_vsync_cbr(int interactive)
{
    if (gui_menu_shown()) return 0;
    if (!get_global_draw()) return 0;
    if (digic_zoom_overlay_enabled()) return 0; 
    if (!lv_luma_is_accurate()) return 0;
	
    astro_step();

    return CBR_RET_CONTINUE;
}

static int off_star = 0;
static int off_pol = 0;

static MENU_UPDATE_FUNC(starfocus_update)
{
    if (!starfocus_enabled) return;

    switch (starfocus_mode)
    {
    case 0:
        MENU_SET_VALUE("RGB");
        break;
    case 1:
        MENU_SET_VALUE("RAW");
        break;
    }

    off_star = 1;
    off_pol ? (polal_enabled = 0, off_pol = 0) : 0;
}

static MENU_UPDATE_FUNC(polalign_update)
{
    if (!polal_enabled) return;

    off_pol = 1;
    off_star ? (starfocus_enabled = 0, off_star = 0) : 0;

}

static MENU_UPDATE_FUNC(colorscheme_update)
{
    color_scheme > 0 ? (bmp_color_scheme = 0) : (bmp_color_scheme = 4);
}

static struct menu_entry astro_menu[] = {
    {
        .name = "Star Focus",
        .priv = &starfocus_enabled,
        .max = 1,
        .depends_on = DEP_LIVEVIEW | DEP_GLOBAL_DRAW,
        .update = starfocus_update,
        .help = "Perfect star focus using Half Flux Diameter formula",
        .children = (struct menu_entry[])
        {
            {
                .name = "Mode",
                .priv = &starfocus_mode,
                .max = 1,
                .help = "Choose the HFD mode:",
                .help2 =
                "Compute Half Flux Diameter using 8-bit (0-255) data.\n"
                "Compute Half Flux Diameter using RAW data (more accurate)\n",
                .choices = CHOICES("0..255", "RAW"),
                .icon_type = IT_DICE,
            },
            {
                .name = "Star threshold",
                .priv = &starfocus_starlevel,
                .select = starfocus_adjust_star_level,
                .max = 15,
                .icon_type = IT_PERCENT_LOG,
                .unit = UNIT_PERCENT,
                .help = "How many pixels are considered as bright pixels (%).",
                .help2 = "The bright pixels constitute the stars.",
            },
            {
                .name = "Background level",
                .priv = &starfocus_bgrlevel,
                .select = starfocus_adjust_bgr_level,
                .max = 70,
                .icon_type = IT_PERCENT_LOG,
                .unit = UNIT_PERCENT,
                .help = "How many pixels are considered as background pixels (%).",
                .help2 = "Background pixels are usually sky and noise pixels.",
            },
            MENU_EOL,
        },
    },
    {
        .name = "Polar Alignment",
        .priv = &polal_enabled,
        .max = 1,
        .depends_on = DEP_LIVEVIEW | DEP_GLOBAL_DRAW,
        .update = polalign_update,
        .help = "Polar alignment of your EQ mount",
        .children = (struct menu_entry[])
        {
            {
                .name = "Hemisphere",
                .priv = &polal_hemisphere,
                .max = 1,
                .choices = (const char *[])
                {"Northern", "Southern"},
                .icon_type = IT_DICE,
                .help = "Which hemisphere are you on?",
            },
            {
                .name = "Tutorials",
                .priv = &polal_tutorial,
                .max = 1,
                .choices = (const char *[])
                {"OFF", "ON"},
                .icon_type = IT_DICE,
                .help = "Some kind of user's manual",
            },
            MENU_EOL
        }
    },
    {
        .name = "Color scheme",
        .priv = &color_scheme,
        .update = colorscheme_update,
        .max = 1,
        .depends_on = DEP_LIVEVIEW | DEP_GLOBAL_DRAW,
        .help = "You can change color scheme here too!",
        .choices = CHOICES("Dark Red", "Default"),
        .icon_type = IT_DICE,
    }

};

static unsigned int astro_init()
{
    //menu_add("Astro", astro_menu, COUNT(astro_menu));
    menu_add("Focus", astro_menu, COUNT(astro_menu));
    return 0;
}

static unsigned int astro_deinit()
{
    return 0;
}

MODULE_INFO_START()
MODULE_INIT(astro_init)
MODULE_DEINIT(astro_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
MODULE_CBR(CBR_SECONDS_CLOCK, astro_vsync_cbr, 0)
MODULE_CBR(CBR_KEYPRESS, astro_keypress_cbr, 0)
MODULE_CBRS_END()

MODULE_CONFIGS_START()
MODULE_CONFIG(starfocus_enabled)
MODULE_CONFIG(starfocus_mode)
MODULE_CONFIG(starfocus_starlevel)
MODULE_CONFIG(starfocus_bgrlevel)
MODULE_CONFIG(polal_enabled)
MODULE_CONFIG(polal_hemisphere)
MODULE_CONFIG(polal_tutorial)
MODULE_CONFIG(color_scheme)
MODULE_CONFIGS_END()