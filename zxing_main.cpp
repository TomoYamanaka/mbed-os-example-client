
#include "mbed.h"
#include "EasyAttach_CameraAndLCD.h"
#include "ImageReaderSource.h"
#include "AsciiFont.h"

/**** User Selection *********/
/** JPEG out setting **/
#define JPEG_SEND              (1)                 /* Select  0(JPEG images are not output to PC) or 1(JPEG images are output to PC on USB(CDC) for focusing the camera) */
#define JPEG_ENCODE_QUALITY    (75)                /* JPEG encode quality (min:1, max:75 (Considering the size of JpegBuffer, about 75 is the upper limit.)) */
#define VFIELD_INT_SKIP_CNT    (0)                 /* A guide for GR-LYCHEE.  0:60fps, 1:30fps, 2:20fps, 3:15fps, 4:12fps, 5:10fps */
/** Decode hints **/
#define DECODE_HINTS           (DecodeHints::ONED_HINT | DecodeHints::QR_CODE_HINT | DecodeHints::DATA_MATRIX_HINT | DecodeHints::AZTEC_HINT)
/*****************************/

/* Video input and LCD layer 0 output */
#define VIDEO_FORMAT           (DisplayBase::VIDEO_FORMAT_YCBCR422)
#define GRAPHICS_FORMAT        (DisplayBase::GRAPHICS_FORMAT_YCBCR422)
//#define WR_RD_WRSWA            (DisplayBase::WR_RD_WRSWA_32_16BIT)
#define WR_RD_WRSWA            (DisplayBase::WR_RD_WRSWA_16BIT)
#define DATA_SIZE_PER_PIC      (2u)

/*! Frame buffer stride: Frame buffer stride should be set to a multiple of 32 or 128
    in accordance with the frame buffer burst transfer mode. */
#if MBED_CONF_APP_LCD
  #define VIDEO_PIXEL_HW       LCD_PIXEL_WIDTH   /* QVGA */
  #define VIDEO_PIXEL_VW       LCD_PIXEL_HEIGHT  /* QVGA */
#else
  #define VIDEO_PIXEL_HW       (640u)  /* VGA */
  #define VIDEO_PIXEL_VW       (480u)  /* VGA */
#endif

#define FRAME_BUFFER_STRIDE    (((VIDEO_PIXEL_HW * DATA_SIZE_PER_PIC) + 31u) & ~31u)
#define FRAME_BUFFER_HEIGHT    (VIDEO_PIXEL_VW)

DisplayBase Display;
// Timer
static Timer decode_timer;

#if defined(__ICCARM__)
#pragma data_alignment=32
static uint8_t user_frame_buffer0[FRAME_BUFFER_STRIDE * FRAME_BUFFER_HEIGHT]@ ".mirrorram";
#else
static uint8_t user_frame_buffer0[FRAME_BUFFER_STRIDE * FRAME_BUFFER_HEIGHT]__attribute((section("NC_BSS"),aligned(32)));
#endif

#if JPEG_SEND
#include "JPEG_Converter.h"
#include "DisplayApp.h"
#include "dcache-control.h"

#if defined(__ICCARM__)
#pragma data_alignment=32
static uint8_t JpegBuffer[2][1024 * 64];
#else
static uint8_t JpegBuffer[2][1024 * 64]__attribute((aligned(32)));
#endif
static size_t jcu_encode_size[2];
static JPEG_Converter Jcu;
static int jcu_buf_index_write = 0;
static int jcu_buf_index_write_done = 0;
static int jcu_buf_index_read = 0;
static volatile int jcu_encoding = 0;
static volatile int image_change = 0;
static DisplayApp  display_app;
static int Vfield_Int_Cnt = 0;

static int decode_wait_time = 0;
static void (*p_callback_func)(char * addr, int size);

static void JcuEncodeCallBackFunc(JPEG_Converter::jpeg_conv_error_t err_code) {
    if (err_code == JPEG_Converter::JPEG_CONV_OK) {
        jcu_buf_index_write_done = jcu_buf_index_write;
        image_change = 1;
    }
    jcu_encoding = 0;
}

static void snapshot(void) {
    while ((jcu_encoding == 1) || (image_change == 0)) {
        Thread::wait(1);
    }
    jcu_buf_index_read = jcu_buf_index_write_done;
    image_change = 0;
    display_app.SendJpeg(JpegBuffer[jcu_buf_index_read], (int)jcu_encode_size[jcu_buf_index_read]);
}

static void IntCallbackFunc_Vfield(DisplayBase::int_type_t int_type) {
    if (Vfield_Int_Cnt < VFIELD_INT_SKIP_CNT) {
        Vfield_Int_Cnt++;
        return;
    }
    Vfield_Int_Cnt = 0;

    //Interrupt callback function
    if (jcu_encoding == 0) {
        JPEG_Converter::bitmap_buff_info_t bitmap_buff_info;
        JPEG_Converter::encode_options_t   encode_options;

        bitmap_buff_info.width              = VIDEO_PIXEL_HW;
        bitmap_buff_info.height             = VIDEO_PIXEL_VW;
        bitmap_buff_info.format             = JPEG_Converter::WR_RD_YCbCr422;
        bitmap_buff_info.buffer_address     = (void *)user_frame_buffer0;

        encode_options.encode_buff_size     = sizeof(JpegBuffer[0]);
        encode_options.p_EncodeCallBackFunc = &JcuEncodeCallBackFunc;
//        encode_options.input_swapsetting    = JPEG_Converter::WR_RD_WRSWA_32_16_8BIT;
        encode_options.input_swapsetting    = JPEG_Converter::WR_RD_WRSWA_16_8BIT;

        jcu_encoding = 1;
        if (jcu_buf_index_read == jcu_buf_index_write) {
            jcu_buf_index_write ^= 1;  // toggle
        }
        jcu_encode_size[jcu_buf_index_write] = 0;
        dcache_invalid(JpegBuffer[jcu_buf_index_write], sizeof(JpegBuffer[0]));
        if (Jcu.encode(&bitmap_buff_info, JpegBuffer[jcu_buf_index_write],
            &jcu_encode_size[jcu_buf_index_write], &encode_options) != JPEG_Converter::JPEG_CONV_OK) {
            jcu_encode_size[jcu_buf_index_write] = 0;
            jcu_encoding = 0;
        }
    }
}
#endif

static void Start_Video_Camera(void) {
    // Video capture setting (progressive form fixed)
    Display.Video_Write_Setting(
        DisplayBase::VIDEO_INPUT_CHANNEL_0,
        DisplayBase::COL_SYS_NTSC_358,
        (void *)user_frame_buffer0,
        FRAME_BUFFER_STRIDE,
        VIDEO_FORMAT,
        WR_RD_WRSWA,
        VIDEO_PIXEL_VW,
        VIDEO_PIXEL_HW
    );
    EasyAttach_CameraStart(Display, DisplayBase::VIDEO_INPUT_CHANNEL_0);
}

#if MBED_CONF_APP_LCD
static void Start_LCD_Display(void) {
    DisplayBase::rect_t rect;

    rect.vs = 0;
    rect.vw = LCD_PIXEL_HEIGHT;
    rect.hs = 0;
    rect.hw = LCD_PIXEL_WIDTH;
    Display.Graphics_Read_Setting(
        DisplayBase::GRAPHICS_LAYER_0,
        (void *)user_frame_buffer0,
        FRAME_BUFFER_STRIDE,
        GRAPHICS_FORMAT,
        WR_RD_WRSWA,
        &rect
    );
    Display.Graphics_Start(DisplayBase::GRAPHICS_LAYER_0);

    Thread::wait(50);
    EasyAttach_LcdBacklight(true);
}

#define VIDEO_PIXEL_HW_STR              (VIDEO_PIXEL_HW - 64)
#define VIDEO_PIXEL_VW_STR              (VIDEO_PIXEL_VW - 64)
#define FRAME_BUFFER_BYTE_PER_PIXEL_STR (2u)
#define FRAME_BUFFER_STRIDE_STR         (((VIDEO_PIXEL_HW_STR * FRAME_BUFFER_BYTE_PER_PIXEL_STR) + 31u) & ~31u)

static uint8_t user_frame_buffer_string[FRAME_BUFFER_STRIDE_STR * VIDEO_PIXEL_VW_STR]__attribute((section("NC_BSS"),aligned(32)));
static AsciiFont ascii_font(user_frame_buffer_string, VIDEO_PIXEL_HW_STR, VIDEO_PIXEL_VW_STR, FRAME_BUFFER_STRIDE_STR, FRAME_BUFFER_BYTE_PER_PIXEL_STR);
static bool      string_draw;
static int error_cnt;

static void decode_string_init(void) {
    DisplayBase::rect_t rect;

    /* The layer by which the touch panel location is drawn */
    ascii_font.Erase(0x00000000);  /* rrrrGBAR (r:Reserve G:Green B:Blue A:Alpha R:Red */
    rect.vs = 32;
    rect.vw = VIDEO_PIXEL_VW_STR;
    rect.hs = 32;
    rect.hw = VIDEO_PIXEL_HW_STR;
    Display.Graphics_Read_Setting(
        DisplayBase::GRAPHICS_LAYER_2,
        (void *)user_frame_buffer_string,
        FRAME_BUFFER_STRIDE_STR,
        DisplayBase::GRAPHICS_FORMAT_ARGB4444,
        DisplayBase::WR_RD_WRSWA_32_16BIT,
        &rect
    );
    Display.Graphics_Start(DisplayBase::GRAPHICS_LAYER_2);
    string_draw = false;
    error_cnt = 0;
}

static void decode_string_disp(char ** decode_str) {
    if ((decode_str != NULL) && (*decode_str != NULL)) {
        /* Drow string */
        ascii_font.Erase(0x00000090);  /* rrrrGBAR (r:Reserve G:Green B:Blue A:Alpha R:Red */
        int rest_size = strlen(*decode_str);
        int draw_idx = 0;
        int draw_size;
        int draw_line = 0;

        while (rest_size > 0) {
            draw_size = ascii_font.DrawStr(*decode_str + draw_idx, 6, 5 + (18 * draw_line), 0x0000ffff, 2);
            if (draw_size <= 0) {
                break;
            }
            rest_size -= draw_size;
            draw_idx += draw_size;
            draw_line++;
        }
        string_draw = true;
        error_cnt = 0;
    } else {
        if (string_draw != false) {
            error_cnt++;
            if (error_cnt > 15) {
                /* Clear string */
                ascii_font.Erase(0x00000000);  /* rrrrGBAR (r:Reserve G:Green B:Blue A:Alpha R:Red */
                string_draw = false;
            }
        }
    }
}
#endif


/****** zxing_init ******/
void zxing_init(void (*pfunc)(char * addr, int size)) {
    // Initialize the background to black
    for (int i = 0; i < (int)sizeof(user_frame_buffer0); i += 2) {
        user_frame_buffer0[i + 0] = 0x10;
        user_frame_buffer0[i + 1] = 0x80;
    }

#if MBED_CONF_APP_LCD
    EasyAttach_Init(Display, 640, 360);
#else
    EasyAttach_Init(Display);
#endif
#if JPEG_SEND
    Jcu.SetQuality(JPEG_ENCODE_QUALITY);
    // Interrupt callback function setting (Field end signal for recording function in scaler 0)
    Display.Graphics_Irq_Handler_Set(DisplayBase::INT_TYPE_S0_VFIELD, 0, IntCallbackFunc_Vfield);
#endif
    Start_Video_Camera();
#if MBED_CONF_APP_LCD
    decode_string_init();
    Start_LCD_Display();
#endif

    p_callback_func = pfunc;
    decode_timer.reset();
    decode_timer.start();
}

/****** zxing_main ******/
int zxing_loop() {
    int decode_result = -1;
    vector<Ref<Result> > results;
    char ** decode_str = NULL;

    /* Decode barcode image */
    if (decode_timer.read_ms() >= decode_wait_time) {
        decode_timer.reset();
        DecodeHints hints(DECODE_HINTS);
        hints.setTryHarder(false);
        decode_result = ex_decode(user_frame_buffer0, (FRAME_BUFFER_STRIDE * VIDEO_PIXEL_VW), VIDEO_PIXEL_HW, VIDEO_PIXEL_VW, &results, hints);
        if (decode_result == 0) {
            decode_str = (char **)&(results[0]->getText()->getText());
            int size = strlen(*decode_str);
            if (p_callback_func != NULL) {
                p_callback_func(*decode_str, size);
            }
            decode_wait_time = 500;
        } else {
            decode_wait_time = 10;
        }
#if MBED_CONF_APP_LCD
        decode_string_disp(decode_str);
#endif
    }

    return decode_result;
}

