/*
 * Copyright (c) 2017 Sugizaki Yukimasa (ysugi@idein.jp)
 * All rights reserved.
 *
 * This software is licensed under a Modified (3-Clause) BSD License.
 * You should have received a copy of this license along with this
 * software. If not, contact the copyright holder above.
 */

#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_util_params.h>
#include <interface/mmal/util/mmal_connection.h>
#include <interface/mmal/util/mmal_component_wrapper.h>
#include <interface/mmal/util/mmal_default_components.h>
#include "rpigrafx.h"
#include "local.h"
#include "config.h"

#ifdef HAVE_RPICAM
#include <rpicam.h>
#endif /* HAVE_RPICAM */

#ifdef HAVE_RPIRAW
#include <rpiraw.h>
#endif /* HAVE_RPICAM */

#if defined(HAVE_RPICAM) && defined(HAVE_RPIRAW)
#define IMPL_RAWCAM 1
#endif /* defined(HAVE_RPICAM) && defined(HAVE_RPIRAW) */


#define MAX_CAMERAS          MMAL_PARAMETER_CAMERA_INFO_MAX_CAMERAS
#define NUM_SPLITTER_OUTPUTS 4
#define CAMERA_PREVIEW_PORT 0
#define CAMERA_CAPTURE_PORT 2

static int32_t num_cameras = 0;

/*
 * ** Components connections **
 *
 * name:   A normal component.
 * name#:  A component using wrapper.
 * [n]:    Port number of a component.
 * (func): A userland function that does format or color conversion.
 * /:      Tunneled connection.
 * |:      Connection by using a shared port pool.
 * !:      Connection by using separate (per-port) port pools.
 *
 * When camera->output[0] (preview port) is used as a capture port.
 *            camera
 *             [0]
 *              /
 *             [0]
 *           splitter
 *   [0]    [1]    [2]    [3]
 *    /      /      /      /
 *   [0]    [0]    [0]    [0]
 *   isp    isp    isp    isp
 *   [0]    [0]    [0]    [0]
 *    |      |      |      |
 *  (edit) (edit) (edit) (edit)
 *    |      |      |      |
 *   [0]    [0]    [0]    [0]
 *  render render render render
 *
 * When vc.ril.camera->output[2] (capture port) is used as a capture port,
 * the preview port (camera->output[0]) is still used for AWB processing.
 *                    camera
 *             [2]              [0]
 *              /                /
 *             [0]              [0]
 *           splitter           null
 *   [0]    [1]    [2]    [3]
 *    /      /      /      /
 *   [0]    [0]    [0]    [0]
 *   isp    isp    isp    isp
 *   [0]    [0]    [0]    [0]
 *    |      |      |      |
 *  (edit) (edit) (edit) (edit)
 *    |      |      |      |
 *   [0]    [0]    [0]    [0]
 *  render render render render
 *
 * When vc.ril.rawcam->output[0] is used as a capture port,
 * camera control via I2C and hardware-side AWB processing is done by librpicam
 * and demosaicing and software-side AWB processing are done by librpiraw.
 *           rawcam#
 *             [0]
 *              !
 *          (demosaic)
 *              !
 *             [0]
 *          splitter#
 *   [0]    [1]    [2]    [3]
 *    /      /      /      /
 *   [0]    [0]    [0]    [0]
 *   isp    isp    isp    isp
 *   [0]    [0]    [0]    [0]
 *    |      |      |      |
 *  (edit) (edit) (edit) (edit)
 *    |      |      |      |
 *   [0]    [0]    [0]    [0]
 *  render render render render
 *
 * However, rawcam is used and use_isp_for_demosaicing is set,
 * another isp instance is used for demosaicing.
 *            rawcam
 *             [0]
 *              /
 *             [0]
 *             isp
 *             [0]
 *              /
 *             [0]
 *           splitter
 *   [0]    [1]    [2]    [3]
 *    /      /      /      /
 *   [0]    [0]    [0]    [0]
 *   isp    isp    isp    isp
 *   [0]    [0]    [0]    [0]
 *    |      |      |      |
 *  (edit) (edit) (edit) (edit)
 *    |      |      |      |
 *   [0]    [0]    [0]    [0]
 *  render render render render
 */

/*
 * Because raw image from camera won't be directly passed to render, we need to
 * allocate port pools for rawcam->output[0] and splitter->input[0] manually,
 * which was done by mmal_connection_create() or in the firmware. Allocating the
 * pools can be achieved by calling the raw MMAL funcs, but it's easier to use
 * the wrapper.
 */

static MMAL_COMPONENT_T *cp_cameras[MAX_CAMERAS];
#ifdef IMPL_RAWCAM
static MMAL_WRAPPER_T *cpw_rawcams[MAX_CAMERAS];
#endif /* IMPL_RAWCAM */
static MMAL_COMPONENT_T *cp_splitters[MAX_CAMERAS];
static MMAL_WRAPPER_T *cpw_splitters[MAX_CAMERAS];
static MMAL_COMPONENT_T *cp_nulls[MAX_CAMERAS];
static MMAL_COMPONENT_T *cp_isps[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];
static MMAL_COMPONENT_T *cp_renders[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];
static MMAL_CONNECTION_T *conn_camera_nulls[MAX_CAMERAS];
static MMAL_CONNECTION_T *conn_camera_splitters[MAX_CAMERAS];
static MMAL_CONNECTION_T *conn_splitters_isps[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];
static MMAL_CONNECTION_T *conn_isps_renders[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];

static struct cameras_config {
    _Bool is_used;
    int32_t width, height;
    int32_t max_width, max_height;
    unsigned camera_output_port_index;
    _Bool use_camera_capture_port;

    struct splitter_config {
        int next_output_idx;
    } splitter;
    struct isp_config {
        int32_t width, height;
        MMAL_FOURCC_T encoding;
        _Bool is_zero_copy_rendering;
    } isp[NUM_SPLITTER_OUTPUTS];
    struct render_config {
        MMAL_DISPLAYREGION_T region;
    } render[NUM_SPLITTER_OUTPUTS];

    _Bool is_rawcam;
#ifdef IMPL_RAWCAM
    MMAL_FOURCC_T raw_encoding;
    rpigrafx_rawcam_camera_model_t rawcam_camera_model;
    unsigned nbits_of_raw_from_camera;
    MMAL_PARAMETER_CAMERA_RX_CONFIG_T rx_cfg;
    union {
        struct rpicam_imx219_config imx219;
    } rpicam_config;
#endif /* IMPL_RAWCAM */
} cameras_config[MAX_CAMERAS];
static struct callback_context *ctxs[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];

#define WARN_HEADER(pre, header, post) \
    do { \
        if (header != NULL) { \
            print_error("%s%p %p %d 0x%08x%s", \
                        pre, \
                        header, header->data, header->length, header->flags, \
                        post); \
       } else { \
            print_error("%s%p%s", pre, header, post); \
       } \
    } while (0)

static MMAL_STATUS_T config_port(MMAL_PORT_T *port,
                                 const MMAL_FOURCC_T encoding,
                                 const int32_t width, const int32_t height)
{
    port->format->encoding = encoding;
    port->format->es->video.width  = VCOS_ALIGN_UP(width,  32);
    port->format->es->video.height = VCOS_ALIGN_UP(height, 16);
    port->format->es->video.crop.x = 0;
    port->format->es->video.crop.y = 0;
    port->format->es->video.crop.width  = width;
    port->format->es->video.crop.height = height;
    return mmal_port_format_commit(port);
}

static MMAL_STATUS_T config_port_crop(MMAL_PORT_T *port,
                                      const MMAL_FOURCC_T encoding,
                                      const int32_t actual_width,
                                      const int32_t actual_height,
                                      const int32_t crop_width,
                                      const int32_t crop_height)
{
    port->format->encoding = encoding;
    port->format->es->video.width  = VCOS_ALIGN_UP(actual_width,  32);
    port->format->es->video.height = VCOS_ALIGN_UP(actual_height, 16);
    port->format->es->video.crop.x = 0;
    port->format->es->video.crop.y = 0;
    port->format->es->video.crop.width  = crop_width;
    port->format->es->video.crop.height = crop_height;
    return mmal_port_format_commit(port);
}

int priv_rpigrafx_mmal_init()
{
    int i, j;
    int ret = 0;
    MMAL_STATUS_T status;

    if (priv_rpigrafx_called.mmal != 0)
        goto end;

    for (i = 0; i < MAX_CAMERAS; i ++) {
        struct cameras_config *cfg = &cameras_config[i];
        cp_cameras[i] = NULL;
        cfg->is_used = 0;
        cfg->is_rawcam = 0;
        if ((ret = rpigrafx_config_camera_port(i,
                                               RPIGRAFX_CAMERA_PORT_PREVIEW)))
            goto end;

        cp_splitters[i] = NULL;
        cfg->splitter.next_output_idx = 0;
        conn_camera_splitters[i] = NULL;

        for (j = 0; j < NUM_SPLITTER_OUTPUTS; j ++) {
            cp_isps[i][j] = NULL;
            conn_splitters_isps[i][j] = NULL;
        }
    }

    {
        MMAL_COMPONENT_T *cp_camera_info = NULL;
        MMAL_PARAMETER_CAMERA_INFO_T camera_info = {
            .hdr = {
                .id = MMAL_PARAMETER_CAMERA_INFO,
                .size = sizeof(camera_info)
            }
        };

        status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO,
                                       &cp_camera_info);
        if (status != MMAL_SUCCESS) {
            print_error("Creating camera_info component failed: 0x%08x", status);
            cp_camera_info = NULL;
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_get(cp_camera_info->control, &camera_info.hdr);
        if (status != MMAL_SUCCESS) {
            print_error("Getting camera info failed: 0x%08x", status);
            ret = 1;
            goto end;
        }

        num_cameras = camera_info.num_cameras;
        if (num_cameras <= 0) {
            print_error("No cameras found: 0x%08x", num_cameras);
            ret = 1;
            goto end;
        }

        for (i = 0; i < num_cameras; i ++) {
            struct cameras_config *cfg = &cameras_config[i];
            cfg->max_width  = camera_info.cameras[i].max_width;
            cfg->max_height = camera_info.cameras[i].max_height;
        }
        for (; i < MAX_CAMERAS; i ++) {
            struct cameras_config *cfg = &cameras_config[i];
            cfg->max_width  = 0;
            cfg->max_height = 0;
        }

        status = mmal_component_destroy(cp_camera_info);
        if (status != MMAL_SUCCESS) {
            print_error("Destroying camera_info component failed: 0x%08x", status);
            cp_camera_info = NULL;
            ret = 1;
            goto end;
        }
    }

end:
    priv_rpigrafx_called.mmal ++;

    return ret;
}

int priv_rpigrafx_mmal_finalize()
{
    int i, j;
    int ret = 0;

    if (priv_rpigrafx_called.mmal != 1)
        goto skip;

    for (i = 0; i < MAX_CAMERAS; i ++) {
        struct cameras_config *cfg = &cameras_config[i];
        cp_cameras[i] = cp_splitters[i] = NULL;
        for (j = 0; j < NUM_SPLITTER_OUTPUTS; j ++)
            cp_isps[i][j] = NULL;
        cfg->width  = -1;
        cfg->height = -1;
        cfg->max_width  = -1;
        cfg->max_height = -1;
        cfg->splitter.next_output_idx = 0;
    }

skip:
    priv_rpigrafx_called.mmal --;
    return ret;
}

static void callback_control(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *header)
{
    if (priv_rpigrafx_verbose)
        print_error("Called by a port %s", port->name);
    mmal_buffer_header_release(header);
}

static void callback_conn(MMAL_CONNECTION_T *conn)
{
    if (priv_rpigrafx_verbose)
        print_error("Called by a connection %s between %s and %s",
                    conn->name, conn->out->name, conn->in->name);
}

int rpigrafx_config_camera_frame(const int32_t camera_number,
                                 const int32_t width, const int32_t height,
                                 const MMAL_FOURCC_T encoding,
                                 const _Bool is_zero_copy_rendering,
                                 rpigrafx_frame_config_t *fcp)
{
    int32_t max_width, max_height;
    int idx;
    struct cameras_config *cfg = &cameras_config[camera_number];
    struct callback_context *ctx = NULL;
    int ret = 0;

    if (camera_number >= num_cameras) {
        print_error("camera_number(%d) exceeds num_cameras(%d)",
                    camera_number, num_cameras);
        ret = 1;
        goto end;
    }
    max_width  = cfg->max_width;
    max_height = cfg->max_height;
    if (width > max_width) {
        print_error("width(%d) exceeds max_width(%d) of camera %d",
                    width, max_width, camera_number);
        ret = 1;
        goto end;
    } else if (height > max_height) {
        print_error("height(%d) exceeds max_height(%d) of camera %d",
                    width, max_width, camera_number);
        ret = 1;
        goto end;
    }

    /*
     * Only set use flag here.
     * cfg->{width,height}
     * will be set on rpigrafx_finish_config.
     */
    cfg->is_used = !0;

    if (cfg->splitter.next_output_idx == NUM_SPLITTER_OUTPUTS - 1) {
        print_error("Too many splitter clients(%d) of camera %d",
                    cfg->splitter.next_output_idx,
                    camera_number);
        ret = 1;
        goto end;
    }
    idx = cfg->splitter.next_output_idx ++;

    cfg->isp[idx].width  = width;
    cfg->isp[idx].height = height;
    cfg->isp[idx].encoding = encoding;
    cfg->isp[idx].is_zero_copy_rendering = is_zero_copy_rendering;

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        print_error("Failed to allocate context");
        ret = 1;
        goto end;
    }
    ctx->status = MMAL_SUCCESS;
    ctx->header = NULL;
    ctx->is_header_passed_to_render = 0;
    ctxs[camera_number][idx] = ctx;

    fcp->camera_number = camera_number;
    fcp->splitter_output_port_index = idx;
    fcp->is_zero_copy_rendering = is_zero_copy_rendering;
    fcp->ctx = ctx;

end:
    return ret;
}

int rpigrafx_config_rawcam(const rpigrafx_rawcam_camera_model_t camera_model,
                           const MMAL_CAMERA_RX_CONFIG_DECODE decode,
                           const MMAL_CAMERA_RX_CONFIG_ENCODE encode,
                           const MMAL_CAMERA_RX_CONFIG_UNPACK unpack,
                           const MMAL_CAMERA_RX_CONFIG_PACK   pack,
                           const uint32_t data_lanes,
                           const uint32_t nbits_of_raw_from_camera,
                           const rpigrafx_bayer_pattern_t bayer_pattern,
                           rpigrafx_frame_config_t *fcp)
{
#ifdef IMPL_RAWCAM

    uint32_t image_id;
    MMAL_PARAMETER_CAMERA_RX_CONFIG_T rx_cfg;
    MMAL_FOURCC_T encoding = MMAL_FOURCC('\0', '\0', '\0', '\0');
    struct cameras_config *cfg = &cameras_config[fcp->camera_number];
    int ret = 0;

    /*
     * See the MIPI specification for the values. If your one's version is
     * 1.01.00 r0.04 2-Apr-2009, they're on p.88.
     * 6, 7 and 14 is also supported by MIPI but the Raspberry Pi firmware
     * doen't for now.
     */
    switch (nbits_of_raw_from_camera) {
        case 8:
            image_id = 0x2a;
            switch (bayer_pattern) {
                case RPIGRAFX_BAYER_PATTERN_BGGR:
                    encoding = MMAL_ENCODING_BAYER_SBGGR8;
                    break;
                case RPIGRAFX_BAYER_PATTERN_GRBG:
                    encoding = MMAL_ENCODING_BAYER_SGRBG8;
                    break;
                case RPIGRAFX_BAYER_PATTERN_GBRG:
                    encoding = MMAL_ENCODING_BAYER_SGBRG8;
                    break;
                case RPIGRAFX_BAYER_PATTERN_RGGB:
                    encoding = MMAL_ENCODING_BAYER_SRGGB8;
                    break;
            }
            break;
        case 10:
            image_id = 0x2b;
            switch (bayer_pattern) {
                case RPIGRAFX_BAYER_PATTERN_BGGR:
                    encoding = MMAL_ENCODING_BAYER_SBGGR10P;
                    break;
                case RPIGRAFX_BAYER_PATTERN_GRBG:
                    encoding = MMAL_ENCODING_BAYER_SGRBG10P;
                    break;
                case RPIGRAFX_BAYER_PATTERN_GBRG:
                    encoding = MMAL_ENCODING_BAYER_SGBRG10P;
                    break;
                case RPIGRAFX_BAYER_PATTERN_RGGB:
                    encoding = MMAL_ENCODING_BAYER_SRGGB10P;
                    break;
            }
            break;
        case 12:
            image_id = 0x2c;
            switch (bayer_pattern) {
                case RPIGRAFX_BAYER_PATTERN_BGGR:
                    encoding = MMAL_ENCODING_BAYER_SBGGR12P;
                    break;
                case RPIGRAFX_BAYER_PATTERN_GRBG:
                    encoding = MMAL_ENCODING_BAYER_SGRBG12P;
                    break;
                case RPIGRAFX_BAYER_PATTERN_GBRG:
                    encoding = MMAL_ENCODING_BAYER_SGBRG12P;
                    break;
                case RPIGRAFX_BAYER_PATTERN_RGGB:
                    encoding = MMAL_ENCODING_BAYER_SRGGB12P;
                    break;
            }
            break;
        default:
            print_error("Unsupported number of bits of raw from camera: %u",
                        nbits_of_raw_from_camera);
            ret = 1;
            goto end;
    }

    rx_cfg.decode     = decode;
    rx_cfg.encode     = encode;
    rx_cfg.unpack     = unpack;
    rx_cfg.pack       = pack;
    rx_cfg.data_lanes = data_lanes;
    rx_cfg.image_id   = image_id;
    memcpy(&cfg->rx_cfg, &rx_cfg, sizeof(rx_cfg));
    cfg->nbits_of_raw_from_camera = nbits_of_raw_from_camera;
    cfg->rawcam_camera_model = camera_model;
    cfg->is_rawcam = !0;
    cfg->raw_encoding = encoding;

end:
    return ret;

#else /* IMPL_RAWCAM */

    MMAL_PARAM_UNUSED(camera_model);
    MMAL_PARAM_UNUSED(decode);
    MMAL_PARAM_UNUSED(encode);
    MMAL_PARAM_UNUSED(unpack);
    MMAL_PARAM_UNUSED(pack);
    MMAL_PARAM_UNUSED(data_lanes);
    MMAL_PARAM_UNUSED(nbits_of_raw_from_camera);
    MMAL_PARAM_UNUSED(bayer_pattern);
    MMAL_PARAM_UNUSED(fcp);

    print_error("librpicam and librpiraw is needed to use rawcam");
    return 1;

#endif /* IMPL_RAWCAM */
}

int rpigrafx_config_rawcam_imx219(const float exck_freq,
                                  uint_least16_t x, uint_least16_t y,
                                  _Bool orient_hori, _Bool orient_vert,
                                  rpigrafx_rawcam_imx219_binning_mode_t
                                                                   binning_mode,
                                  rpigrafx_frame_config_t *fcp)
{
#ifdef IMPL_RAWCAM

    struct rpicam_imx219_config imx219;
    struct cameras_config *cfg = &cameras_config[fcp->camera_number];
    int ret = 0;

    if (cfg->rawcam_camera_model != RPIGRAFX_RAWCAM_CAMERA_MODEL_IMX219) {
        print_error("rawcam is not configured for IMX219");
        ret = 1;
        goto end;
    }

    rpicam_imx219_default_config(&imx219);
    imx219.exck_freq.num = exck_freq * 1000;
    imx219.exck_freq.den = 1000;
    imx219.temperature_en = !0;
    imx219.num_csi_lanes = cfg->rx_cfg.data_lanes;
    imx219.x = x;
    imx219.y = y;
    imx219.hori_orientation = orient_hori;
    imx219.vert_orientation = orient_vert;
    switch (cfg->nbits_of_raw_from_camera) {
        case 8:
            imx219.comp_enable = !0;
            break;
        case 10:
            imx219.comp_enable = 0;
            break;
        default:
            print_error("IMX219 supports only for raw8 and raw10");
            ret = 1;
            goto end;
    }
    switch (binning_mode) {
        case RPIGRAFX_RAWCAM_IMX219_BINNING_MODE_NONE:
            /* Keep the defaults. */
            break;
    }

    memcpy(&cfg->rpicam_config.imx219, &imx219, sizeof(imx219));

end:
    return ret;

#else /* IMPL_RAWCAM */

    MMAL_PARAM_UNUSED(exck_freq);
    MMAL_PARAM_UNUSED(x);
    MMAL_PARAM_UNUSED(y);
    MMAL_PARAM_UNUSED(orient_hori);
    MMAL_PARAM_UNUSED(orient_vert);
    MMAL_PARAM_UNUSED(binning_mode);
    MMAL_PARAM_UNUSED(fcp);

    print_error("librpicam and librpiraw is needed to use rawcam");
    return 1;

#endif /* IMPL_RAWCAM */
}

int rpigrafx_config_camera_port(const int32_t camera_number,
                                const rpigrafx_camera_port_t camera_port)
{
    struct cameras_config *cfg = &cameras_config[camera_number];
    int ret = 0;

    switch (camera_port) {
        case RPIGRAFX_CAMERA_PORT_PREVIEW:
            cfg->camera_output_port_index = CAMERA_PREVIEW_PORT;
            cfg->use_camera_capture_port = 0;
            break;
        case RPIGRAFX_CAMERA_PORT_CAPTURE:
            cfg->camera_output_port_index = CAMERA_CAPTURE_PORT;
            cfg->use_camera_capture_port = !0;
            break;
        default:
            print_error("Unknown rpigrafx_camera_port_t value: %d\n",
                        camera_port);
            ret = 1;
            goto end;
            break;
    }

end:
    return ret;
}

int rpigrafx_config_camera_frame_render(const _Bool is_fullscreen,
                           const int32_t x, const int32_t y,
                           const int32_t width, const int32_t height,
                           const int32_t layer,
                           rpigrafx_frame_config_t *fcp)
{
    MMAL_DISPLAYREGION_T region = {
        .fullscreen = is_fullscreen,
        .dest_rect = {
            .x = x, .y = y,
            .width = width, .height = height
        },
        .layer = layer,
        .set =   MMAL_DISPLAY_SET_FULLSCREEN
               | MMAL_DISPLAY_SET_DEST_RECT
               | MMAL_DISPLAY_SET_LAYER
    };
    struct cameras_config *cfg = &cameras_config[fcp->camera_number];
    int ret = 0;

    memcpy(&cfg->render[fcp->splitter_output_port_index].region,
           &region, sizeof(region));

    return ret;
}

static int setup_cp_camera_rawcam(const int i,
                                  const int32_t width, const int32_t height)
{
#ifdef IMPL_RAWCAM

    struct cameras_config *cfg = &cameras_config[i];
    MMAL_STATUS_T status;
    int ret = 0;

    status = mmal_wrapper_create(&cpw_rawcams[i], "vc.ril.rawcam");
    if (status != MMAL_SUCCESS) {
        print_error("Creating rawcam component of camera %d failed: 0x%08x",
                    i, status);
        ret = 1;
        goto end;
    }

    switch (cfg->rawcam_camera_model) {
        case RPIGRAFX_RAWCAM_CAMERA_MODEL_IMX219: {
            struct rpicam_imx219_config *stp = &cfg->rpicam_config.imx219;
            stp->width = width;
            stp->height = height;
            if ((ret = rpicam_imx219_open(stp)))
                goto end;
            break;
        }
    }

    {
        MMAL_PORT_T *output = mmal_util_get_port(cpw_rawcams[i]->component,
                                                 MMAL_PORT_TYPE_OUTPUT, 0);
        MMAL_PARAMETER_CAMERA_RX_CONFIG_T rx_cfg = {
            .hdr = {
                .id = MMAL_PARAMETER_CAMERA_RX_CONFIG,
                .size = sizeof(rx_cfg)
            }
        };
        MMAL_FOURCC_T encoding = cfg->raw_encoding;

        if (output == NULL) {
            print_error("Getting output %d of camera %d failed", 0, i);
            ret = 1;
            goto end;
        }

        status = config_port(output, encoding, width, height);
        if (status != MMAL_SUCCESS) {
            print_error("Setting format of camera %d failed: 0x%08x",
                        i, status);
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_get(output, &rx_cfg.hdr);
        if (status != MMAL_SUCCESS) {
            print_error("Getting rx_cfg of rawcam %d failed: 0x%08x",
                        i, status);
            ret = 1;
            goto end;
        }

        /* Use default values for encode_block_length and embedded_data_lines. */
        rx_cfg.decode     = cfg->rx_cfg.decode;
        rx_cfg.encode     = cfg->rx_cfg.encode;
        rx_cfg.unpack     = cfg->rx_cfg.unpack;
        rx_cfg.pack       = cfg->rx_cfg.pack;
        rx_cfg.data_lanes = cfg->rx_cfg.data_lanes;
        rx_cfg.image_id   = cfg->rx_cfg.image_id;
        status = mmal_port_parameter_set(output, &rx_cfg.hdr);
        if (status != MMAL_SUCCESS) {
            print_error("Setting rx_cfg of rawcam %d failed: 0x%08x",
                        i, status);
            ret = 1;
            goto end;
        }

        status = mmal_wrapper_port_enable(output,
                                          MMAL_WRAPPER_FLAG_PAYLOAD_ALLOCATE);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling rawcam component of camera %d failed: 0x%08x",
                        i, status);
            ret = 1;
            goto end;
        }

        MMAL_BUFFER_HEADER_T *header = NULL;
        while ((header = mmal_queue_get(cpw_rawcams[i]->output_pool[0]->queue)) != NULL)
            mmal_port_send_buffer(output, header);
    }

end:
    return ret;

#else /* IMPL_RAWCAM */

    MMAL_PARAM_UNUSED(i);
    MMAL_PARAM_UNUSED(width);
    MMAL_PARAM_UNUSED(height);

    print_error("librpicam and librpiraw is needed to use rawcam");
    return 1;

#endif /* IMPL_RAWCAM */
}

static int setup_cp_camera(const int i,
                           const int32_t width, const int32_t height,
                           const _Bool setup_preview_port_for_null)
{
    struct cameras_config *cfg = &cameras_config[i];
    const unsigned camera_output_port_index = cfg->camera_output_port_index;
    MMAL_STATUS_T status;
    int ret = 0;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &cp_cameras[i]);
    if (status != MMAL_SUCCESS) {
        print_error("Creating camera component of camera %d failed: 0x%08x",
                    i, status);
        ret = 1;
        goto end;
    }
    {
        MMAL_PORT_T *control = mmal_util_get_port(cp_cameras[i],
                                                  MMAL_PORT_TYPE_CONTROL, 0);

        if (control == NULL) {
            print_error("Getting control port of camera %d failed", i);
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_set_int32(control, MMAL_PARAMETER_CAMERA_NUM, i);
        if (status != MMAL_SUCCESS) {
            print_error("Setting camera_num of camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }

        status = mmal_port_enable(control, callback_control);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling control port of camera %d failed: 0x%08x",
                        i, status);
            ret = 1;
            goto end;
        }
    }
    if (setup_preview_port_for_null) {
        MMAL_PORT_T *output = mmal_util_get_port(cp_cameras[i],
                                                 MMAL_PORT_TYPE_OUTPUT,
                                                 CAMERA_PREVIEW_PORT);

        if (output == NULL) {
            print_error("Getting output %d of camera %d failed",
                        CAMERA_PREVIEW_PORT, i);
            ret = 1;
            goto end;
        }

        status = config_port(output, MMAL_ENCODING_OPAQUE, width, height);
        if (status != MMAL_SUCCESS) {
            print_error("Setting format of camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_set_boolean(output,
                                            MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
        if (status != MMAL_SUCCESS) {
            print_error("Setting zero-copy on camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
    }
    {
        MMAL_PORT_T *output = mmal_util_get_port(cp_cameras[i],
                                                 MMAL_PORT_TYPE_OUTPUT,
                                                 camera_output_port_index);

        if (output == NULL) {
            print_error("Getting output %d of camera %d failed",
                        camera_output_port_index, i);
            ret = 1;
            goto end;
        }

        status = config_port(output, MMAL_ENCODING_RGB24, width, height);
        if (status != MMAL_SUCCESS) {
            print_error("Setting format of camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_set_boolean(output,
                                            MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
        if (status != MMAL_SUCCESS) {
            print_error("Setting zero-copy on camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
    }
    status = mmal_component_enable(cp_cameras[i]);
    if (status != MMAL_SUCCESS) {
        print_error("Enabling camera component of camera %d failed: 0x%08x",
                    i, status);
        ret = 1;
        goto end;
    }

end:
    return ret;
}

static int setup_cp_null(const int i,
                         const int32_t width, const int32_t height)
{
    MMAL_STATUS_T status;
    int ret = 0;

    status = mmal_component_create("vc.ril.null_sink", &cp_nulls[i]);
    if (status != MMAL_SUCCESS) {
        print_error("Creating null component of camera %d failed: 0x%08x",
                    i, status);
        ret = 1;
        goto end;
    }
    {
        MMAL_PORT_T *control = mmal_util_get_port(cp_nulls[i],
                                                  MMAL_PORT_TYPE_CONTROL, 0);

        if (control == NULL) {
            print_error("Getting control port of null %d failed", i);
            ret = 1;
            goto end;
        }

        status = mmal_port_enable(control, callback_control);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling control port of null %d failed: 0x%08x",
                        i, status);
            ret = 1;
            goto end;
        }
    }
    {
        MMAL_PORT_T *input = mmal_util_get_port(cp_nulls[i],
                                                MMAL_PORT_TYPE_INPUT, 1);

        if (input == NULL) {
            print_error("Getting input port of null %d failed", i);
            ret = 1;
            goto end;
        }

        status = config_port(input, MMAL_ENCODING_OPAQUE, width, height);
        if (status != MMAL_SUCCESS) {
            print_error("Setting format of null %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_set_boolean(input,
                                                 MMAL_PARAMETER_ZERO_COPY,
                                                 MMAL_TRUE);
        if (status != MMAL_SUCCESS) {
            print_error("Setting zero-copy on null %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
    }
    status = mmal_component_enable(cp_nulls[i]);
    if (status != MMAL_SUCCESS) {
        print_error("Enabling null component of camera %d failed: 0x%08x",
                    i, status);
        ret = 1;
        goto end;
    }

end:
    return ret;
}

static int setup_cp_splitter(const int i, const int len,
                             const int32_t width, const int32_t height,
                             const _Bool is_rawcam)
{
    int j;
    MMAL_COMPONENT_T *component = NULL;
    struct cameras_config *cfg = &cameras_config[i];
    MMAL_STATUS_T status;
    int ret = 0;

    if (!is_rawcam)
        status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_SPLITTER,
                                       &cp_splitters[i]);
    else
        status = mmal_wrapper_create(&cpw_splitters[i],
                                     MMAL_COMPONENT_DEFAULT_VIDEO_SPLITTER);
    if (status != MMAL_SUCCESS) {
        print_error("Creating splitter component of camera %d failed: 0x%08x",
                    i, status);
        ret = 1;
        goto end;
    }

    if (!is_rawcam)
        component = cp_splitters[i];
    else
        component = cpw_splitters[i]->component;

    {
        MMAL_PORT_T *control = mmal_util_get_port(component,
                                                  MMAL_PORT_TYPE_CONTROL, 0);

        if (control == NULL) {
            print_error("Getting control port of splitter %d failed", i);
            ret = 1;
            goto end;
        }

        if (!is_rawcam) {
            status = mmal_port_enable(control, callback_control);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling control port of splitter %d failed: 0x%08x",
                            i, status);
                ret = 1;
                goto end;
            }
        }
    }
    {
        MMAL_PORT_T *input = mmal_util_get_port(component,
                                                MMAL_PORT_TYPE_INPUT, 0);

        if (input == NULL) {
            print_error("Getting input port of splitter %d failed", i);
            ret = 1;
            goto end;
        }

        status = config_port(input, MMAL_ENCODING_RGB24, width, height);
        if (status != MMAL_SUCCESS) {
            print_error("Setting format of " \
                        "splitter %d input failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }

        if (!is_rawcam) {
            status = mmal_port_parameter_set_boolean(input,
                                                     MMAL_PARAMETER_ZERO_COPY,
                                                     MMAL_TRUE);
            if (status != MMAL_SUCCESS) {
                print_error("Setting zero-copy on " \
                            "splitter %d input failed: 0x%08x", i, status);
                ret = 1;
                goto end;
            }
        }

        if (is_rawcam) {
            status = mmal_wrapper_port_enable(input,
                                            MMAL_WRAPPER_FLAG_PAYLOAD_ALLOCATE);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling input port 0 of "
                            "splitter component %d failed: 0x%08x",
                            i, status);
                ret = 1;
                goto end;
            }
        }
    }
    for (j = 0; j < len; j ++) {
        MMAL_PORT_T *output = mmal_util_get_port(component,
                                                 MMAL_PORT_TYPE_OUTPUT, j);
        const int32_t output_width  = cfg->isp[j].width,
                      output_height = cfg->isp[j].height;

        if (output == NULL) {
            print_error("Getting output port of splitter %d,%d failed", i, j);
            ret = 1;
            goto end;
        }

        status = config_port_crop(output, MMAL_ENCODING_RGB24,
                                  width, height,
                                  output_width  * (width  / output_width ),
                                  output_height * (height / output_height));
        if (status != MMAL_SUCCESS) {
            print_error("Setting format of " \
                        "splitter %d output %d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_set_boolean(output,
                                            MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
        if (status != MMAL_SUCCESS) {
            print_error("Setting zero-copy on " \
                        "splitter %d output %d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }

    if (!is_rawcam) {
        status = mmal_component_enable(cp_splitters[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling splitter component of " \
                        "camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
    }

end:
    return ret;
}

static int setup_cp_isp(const int i, const int j,
                        const int32_t width, const int32_t height)
{
    struct cameras_config *cfg = &cameras_config[i];
    MMAL_STATUS_T status;
    int ret = 0;

    status = mmal_component_create("vc.ril.isp", &cp_isps[i][j]);
    if (status != MMAL_SUCCESS) {
        print_error("Creating isp component %d,%d failed: 0x%08x", i, j, status);
        ret = 1;
        goto end;
    }
    {
        MMAL_PORT_T *control = mmal_util_get_port(cp_isps[i][j],
                                                  MMAL_PORT_TYPE_CONTROL, 0);

        if (control == NULL) {
            print_error("Getting control port of isp %d,%d failed", i, j);
            ret = 1;
            goto end;
        }

        status = mmal_port_enable(control, callback_control);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling control port of " \
                        "isp %d,%d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }
    {
        MMAL_PORT_T *input = mmal_util_get_port(cp_isps[i][j],
                                                MMAL_PORT_TYPE_INPUT, 0);
        const int32_t output_width  = cfg->isp[j].width,
                      output_height = cfg->isp[j].height;

        if (input == NULL) {
            print_error("Getting input port of isp %d,%d failed", i, j);
            ret = 1;
            goto end;
        }

        status = config_port_crop(input, MMAL_ENCODING_RGB24, width, height,
                                  output_width  * (width  / output_width ),
                                  output_height * (height / output_height));
        if (status != MMAL_SUCCESS) {
            print_error("Setting format of " \
                        "isp %d input %d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_set_boolean(input,
                                            MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
        if (status != MMAL_SUCCESS) {
            print_error("Setting zero-copy on " \
                        "isp %d input %d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }
    {
        MMAL_PORT_T *output = mmal_util_get_port(cp_isps[i][j],
                                                 MMAL_PORT_TYPE_OUTPUT, 0);

        if (output == NULL) {
            print_error("Getting output port of isp %d,%d failed", i, j);
            ret = 1;
            goto end;
        }

        status = config_port(output, cfg->isp[j].encoding,
                             cfg->isp[j].width, cfg->isp[j].height);
        if (status != MMAL_SUCCESS) {
            print_error("Setting format of " \
                        "isp %d output %d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_set_boolean(output,
                                                 MMAL_PARAMETER_ZERO_COPY,
                                                 MMAL_TRUE);
        if (status != MMAL_SUCCESS) {
            print_error("Setting zero-copy on " \
                        "isp %d output %d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }
    status = mmal_component_enable(cp_isps[i][j]);
    if (status != MMAL_SUCCESS) {
        print_error("Enabling isp component %d,%d failed: 0x%08x",
                    i, j, status);
        ret = 1;
        goto end;
    }

end:
    return ret;
}

static int setup_cp_render(const int i, const int j)
{
    struct cameras_config *cfg = &cameras_config[i];
    MMAL_STATUS_T status;
    int ret = 0;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER,
                                   &cp_renders[i][j]);
    if (status != MMAL_SUCCESS) {
        print_error("Creating render component %d,%d failed: 0x%08x",
                    i, j, status);
        ret = 1;
        goto end;
    }
    {
        MMAL_PORT_T *control = mmal_util_get_port(cp_renders[i][j],
                                                  MMAL_PORT_TYPE_CONTROL, 0);

        if (control == NULL) {
            print_error("Getting control port of render %d,%d failed", i, j);
            ret = 1;
            goto end;
        }

        status = mmal_port_enable(control, callback_control);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling control port of " \
                        "render %d,%d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }
    {
        MMAL_PORT_T *input = mmal_util_get_port(cp_renders[i][j],
                                                MMAL_PORT_TYPE_INPUT, 0);

        if (input == NULL) {
            print_error("Getting input port of render %d,%d failed", i, j);
            ret = 1;
            goto end;
        }

        status = config_port(input,
                             cfg->isp[j].encoding,
                             cfg->isp[j].width,
                             cfg->isp[j].height);
        if (status != MMAL_SUCCESS) {
            print_error("Setting format of " \
                        "render %d input %d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }

        status = mmal_util_set_display_region(input,
                                              &cfg->render[j].region);
        if (status != MMAL_SUCCESS) {
            print_error("Setting region of " \
                        "render %d input %d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }

        status = mmal_port_parameter_set_boolean(input,
                                                 MMAL_PARAMETER_ZERO_COPY,
                                                 MMAL_TRUE);
        if (status != MMAL_SUCCESS) {
            print_error("Setting zero-copy on " \
                        "isp %d input %d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }
    status = mmal_component_enable(cp_renders[i][j]);
    if (status != MMAL_SUCCESS) {
        print_error("Enabling render component %d,%d failed: 0x%08x",
                    i, j, status);
        ret = 1;
        goto end;
    }

end:
    return ret;
}

static int connect_ports(const int i, const int len)
{
    int j;
    struct cameras_config *cfg = &cameras_config[i];
    MMAL_STATUS_T status;
    int ret = 0;

    if (cfg->use_camera_capture_port) {
        /* Connect camera preview port to null for AWB processing. */
        status = mmal_connection_create(&conn_camera_nulls[i],
                                        cp_cameras[i]->
                                                    output[CAMERA_PREVIEW_PORT],
                                        cp_nulls[i]->input[1],
                                        MMAL_CONNECTION_FLAG_TUNNELLING);
        if (status != MMAL_SUCCESS) {
            print_error("Connecting " \
                        "camera and null ports %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
    }

    if (!cfg->is_rawcam) {
        status = mmal_connection_create(&conn_camera_splitters[i],
                                        cp_cameras[i]->
                                          output[cfg->camera_output_port_index],
                                        cp_splitters[i]->input[0],
                                        MMAL_CONNECTION_FLAG_TUNNELLING);
        if (status != MMAL_SUCCESS) {
            print_error("Connecting " \
                        "camera and splitter ports %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
    }

    for (j = 0; j < len; j ++) {
        if (!cfg->is_rawcam)
            status = mmal_connection_create(&conn_splitters_isps[i][j],
                                            cp_splitters[i]->output[j],
                                            cp_isps[i][j]->input[0],
                                            MMAL_CONNECTION_FLAG_TUNNELLING);
        else
            status = mmal_connection_create(&conn_splitters_isps[i][j],
                                            cpw_splitters[i]->output[j],
                                            cp_isps[i][j]->input[0],
                                            MMAL_CONNECTION_FLAG_TUNNELLING);
        if (status != MMAL_SUCCESS) {
            print_error("Connecting "
                        "splitter and isp ports %d,%d failed: 0x%08x",
                        i, j, status);
            ret = 1;
            goto end;
        }
        status = mmal_connection_create(&conn_isps_renders[i][j],
                                        cp_isps[i][j]->output[0],
                                        cp_renders[i][j]->input[0],
                                        0);
        if (status != MMAL_SUCCESS) {
            print_error("Connecting "
                        "isp and render ports %d,%d failed: 0x%08x",
                        i, j, status);
            ret = 1;
            goto end;
        }
    }

    for (j = 0; j < len; j ++) {
        conn_isps_renders[i][j]->callback = callback_conn;
        status = mmal_connection_enable(conn_isps_renders[i][j]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling connection between "
                        "splitter and isp %d,%d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
        conn_splitters_isps[i][j]->callback = callback_conn;
        status = mmal_connection_enable(conn_splitters_isps[i][j]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling connection between "
                        "splitter and isp %d,%d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }
    if (cfg->use_camera_capture_port) {
        conn_camera_nulls[i]->callback = callback_conn;
        status = mmal_connection_enable(conn_camera_nulls[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling connection between "
                        "camera and null %d,%d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }
    if (!cfg->is_rawcam) {
        conn_camera_splitters[i]->callback = callback_conn;
        status = mmal_connection_enable(conn_camera_splitters[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling connection between "
                        "camera and splitter %d,%d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
        }
    }

    for (j = 0; j < len; j ++) {
        MMAL_BUFFER_HEADER_T *header = NULL;
        MMAL_CONNECTION_T *conn = conn_isps_renders[i][j];
        while ((header = mmal_queue_get(conn->pool->queue)) != NULL) {
            status = mmal_port_send_buffer(conn->out, header);
            if (status != MMAL_SUCCESS) {
                print_error("Sending pool buffer to "
                            "isp-render conn %d,%d failed: 0x%08x", status);
                ret = 1;
                goto end;
            }
        }
    }

end:
    return ret;
}

int rpigrafx_finish_config()
{
    int i, j;
    int ret = 0;

    for (i = 0; i < num_cameras; i ++) {
        int len;
        /* Maximum width/height of the requested frames. */
        int32_t max_width, max_height;
        struct cameras_config *cfg = &cameras_config[i];

        if (!cfg->is_used)
            continue;

        len = cfg->splitter.next_output_idx;

        max_width = max_height = 0;
        for (j = 0; j < len; j ++) {
            max_width  = MMAL_MAX(max_width,  cfg->isp[j].width);
            max_height = MMAL_MAX(max_height, cfg->isp[j].height);
        }
#ifdef IMPL_RAWCAM
        if (cfg->is_rawcam) {
            switch (cfg->rawcam_camera_model) {
                case RPIGRAFX_RAWCAM_CAMERA_MODEL_IMX219: {
                    const int32_t mag = MMAL_MIN(cfg->max_width  / max_width,
                                                 cfg->max_height / max_height);
                    max_width  *= mag;
                    max_height *= mag;
                    break;
                }
            }
        }
#endif /* IMPL_RAWCAM */
        cfg->width = max_width;
        cfg->height = max_height;

        if (cfg->is_rawcam) {
            if ((ret = setup_cp_camera_rawcam(i, max_width, max_height)))
                goto end;
        } else {
            if ((ret = setup_cp_camera(i, max_width, max_height,
                                       cfg->use_camera_capture_port)))
                goto end;
        }
        if ((ret = setup_cp_splitter(i, len, max_width, max_height,
                                     cfg->is_rawcam)))
            goto end;
        if (cfg->use_camera_capture_port)
            if ((ret = setup_cp_null(i, max_width, max_height)))
                goto end;
        for (j = 0; j < len; j ++) {
            if ((ret = setup_cp_isp(i, j, max_width, max_height)))
                goto end;
            if ((ret = setup_cp_render(i, j)))
                goto end;
        }
        if ((ret = connect_ports(i, len)))
            goto end;
    }

end:
    return ret;
}

int rpigrafx_capture_next_frame(rpigrafx_frame_config_t *fcp)
{
    struct callback_context *ctx = fcp->ctx;
    struct cameras_config *cfg = &cameras_config[fcp->camera_number];
    int ret = 0;
    MMAL_BUFFER_HEADER_T *header = NULL;
    MMAL_STATUS_T status;

    if (cfg->use_camera_capture_port) {
        status = mmal_port_parameter_set_boolean(cp_cameras[fcp->camera_number]
                                        ->output[cfg->camera_output_port_index],
                                                 MMAL_PARAMETER_CAPTURE,
                                                 MMAL_TRUE);
        if (status != MMAL_SUCCESS) {
            print_error("Setting capture to "
                        "camera %d output %d failed: 0x%08x\n",
                        fcp->camera_number, cfg->camera_output_port_index,
                        status);
            ret = 1;
            goto end;
        }
    }

    ret = rpigrafx_free_frame(fcp);
    if (ret) {
        print_error("rpigrafx_free_frame failed: %d\n", ret);
        return ret;
    }

#ifdef IMPL_RAWCAM
    if (cfg->is_rawcam) {
        const int32_t width = cfg->width,
                      height = cfg->height,
                      /* Stride in header->data. */
                      stride = ALIGN_UP(width, 32),
                      raw_width = rpiraw_width_raw8_to_raw10_rpi(width);
        for (; ; ) {
            MMAL_PORT_T *output = cpw_rawcams[fcp->camera_number]->output[0],
                        *input = cpw_splitters[fcp->camera_number]->input[0];
            MMAL_QUEUE_T *input_queue = cpw_splitters[fcp->camera_number]->
                                                           input_pool[0]->queue;
            uint8_t *raw8 = NULL;

            for (; ; ) {
                _Bool exit_loop = 0;
                status = mmal_wrapper_buffer_get_empty(output, &header, 0);
                switch (status) {
                    case MMAL_SUCCESS:
                        status = mmal_port_send_buffer(output, header);
                        if (status != MMAL_SUCCESS) {
                            print_error("Failed to send empty buffer to rawcam:"
                                        " 0x%08x", status);
                            ret = 1;
                            goto end;
                        }
                        break;
                    case MMAL_EAGAIN:
                        exit_loop = !0;
                        break;
                    default:
                        print_error("Failed to get empty header: 0x%08x",
                                    status);
                        ret = 1;
                        goto end;
                }
                if (exit_loop)
                    break;
            }

            status = mmal_wrapper_buffer_get_full(output, &header,
                                                  MMAL_WRAPPER_FLAG_WAIT);
            if (status != MMAL_SUCCESS) {
                print_error("Failed to get full header from rawcam: 0x%08x",
                            status);
                ret = 1;
                goto end;
            }

            /* Raw info etc... */
            if (header->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) {
                mmal_buffer_header_release(header);
                continue;
            }

            raw8 = malloc(width * height);
            if (raw8 == NULL) {
                print_error("Failed to allocate raw8: %s", strerror(errno));
                ret = 1;
                goto end;
            }

            /* xxx: Add stride argument to this call. */
            ret = rpiraw_convert_raw10_to_raw8(raw8, header->data,
                                               width, height, raw_width);
            if (ret) {
                print_error("rpiraw_convert_raw10_to_raw8: %d", ret);
                goto end;
            }

            mmal_buffer_header_release(header);

            header = mmal_queue_wait(input_queue);
            if (header == NULL) {
                print_error("Failed to wait for header from rawcam");
                ret = 1;
                goto end;
            }

            if (cfg->rawcam_camera_model == RPIGRAFX_RAWCAM_CAMERA_MODEL_IMX219) {
                ret = rpiraw_raw8bggr_component_gain(raw8, width, raw8, width,
                                               width, height, 1.55, 1.0, 1.5);
                if (ret) {
                    print_error("rpiraw_raw8bggr_component_gain: %d", ret);
                    goto end;
                }
            }
            ret = rpiraw_raw8bggr_to_rgb888_nearest_neighbor(header->data,
                                                             stride,
                                                             raw8,
                                                             width, width,
                                                             height);
            if (ret) {
                print_error("rpiraw_raw8bggr_to_rgb888_nearest_neighbor: %d",
                            ret);
                goto end;
            }

            free(raw8);

            {
                uint32_t hist_r[256], hist_g[256], hist_b[256], sum = 0;
                ret = rpiraw_calc_histogram_rgb888(hist_r, hist_g, hist_b,
                                                   header->data,
                                                   stride, width, height);
                sum = hist_r[255] + hist_g[255] + hist_b[255];
                ret = rpicam_imx219_tuner(RPICAM_IMX219_TUNER_METHOD_HEURISTIC,
                                          &cfg->rpicam_config.imx219, sum);
            }

            /*
             * Wait! The header here is not the one the user requested. We pass
             * it to the splitter and wait for the isp to crop them.
             */

            /* xxx: stride * height * 3 ? */
            header->length = width * height * 3;
            header->flags = MMAL_BUFFER_HEADER_FLAG_EOS;
            status = mmal_port_send_buffer(input, header);
            if (status != MMAL_SUCCESS) {
                print_error("Failed to send buffer to splitter: 0x%08x",
                            status);
                ret = 1;
                goto end;
            }

            break;
        }
    }
#endif /* IMPL_RAWCAM */

    for (; ; ) {
        MMAL_CONNECTION_T *conn = conn_isps_renders[fcp->camera_number]
                                              [fcp->splitter_output_port_index];

        while ((header = mmal_queue_get(conn->pool->queue)) != NULL) {
            if (priv_rpigrafx_verbose)
                WARN_HEADER("Got header ", header,
                            " from conn->pool->queue; "
                            "Sending to conn->out");
            mmal_port_send_buffer(conn->out, header);
        }

        header = mmal_queue_wait(conn->queue);
        if (priv_rpigrafx_verbose)
            WARN_HEADER("Got header ", header, " from conn->queue");
        /*
         * camera[2] returns empty queue once every two headers.
         * Retry until we get the full header.
         * It's safe to do this for preview port or rawcam.
         */
        /* xxx: Not to do this here? */
        if (header->length == 0) {
            mmal_buffer_header_release(header);
            continue;
        }
        break;
    }

    ctx->header = header;

end:
    return ret;
}

void* rpigrafx_get_frame(rpigrafx_frame_config_t *fcp)
{
    struct callback_context *ctx = fcp->ctx;
    void *ret = NULL;

    if (ctx->status != MMAL_SUCCESS) {
        print_error("Getting output buffer of isp %d,%d failed: 0x%08x",
                    fcp->camera_number, fcp->splitter_output_port_index,
                    ctx->status);
        ret = NULL;
        goto end;
    }
    if (ctx->header == NULL) {
        print_error("Output buffer of isp %d,%d is NULL",
                    fcp->camera_number, fcp->splitter_output_port_index);
        ret = NULL;
        goto end;
    }
    ret = ctx->header->data;

end:
    return ret;
}

int rpigrafx_free_frame(rpigrafx_frame_config_t *fcp)
{
    struct callback_context *ctx = fcp->ctx;
    int ret = 0;

    if (ctx->header == NULL || ctx->is_header_passed_to_render)
        return 0;

    if (priv_rpigrafx_verbose)
        WARN_HEADER("Releasing header ", ctx->header, "");
    mmal_buffer_header_release(ctx->header);

    ctx->header = NULL;
    ctx->is_header_passed_to_render = 0;

    return ret;
}

int rpigrafx_render_frame(rpigrafx_frame_config_t *fcp)
{
    struct callback_context *ctx = fcp->ctx;
    MMAL_STATUS_T status;
    int ret = 0;

    if (ctx->status != MMAL_SUCCESS) {
        print_error("Getting output buffer of isp %d,%d failed: 0x%08x",
                    fcp->camera_number, fcp->splitter_output_port_index,
                    ctx->status);
        ret = 1;
        goto end;
    }

    status = mmal_port_send_buffer(conn_isps_renders[fcp->camera_number]
                                          [fcp->splitter_output_port_index]->in,
                                   fcp->ctx->header);
    if (status != MMAL_SUCCESS) {
        print_error("Sending header to render failed: 0x%08x", status);
        goto end;
    }

    ctx->is_header_passed_to_render = !0;

end:
    return ret;
}
