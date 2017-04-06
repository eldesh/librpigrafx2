#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_util_params.h>
#include <interface/mmal/util/mmal_connection.h>
#include <interface/mmal/util/mmal_default_components.h>
#include "rpigrafx.h"
#include "local.h"

#define MAX_CAMERAS          MMAL_PARAMETER_CAMERA_INFO_MAX_CAMERAS
#define NUM_SPLITTER_OUTPUTS 4

static int32_t num_cameras = 0;

static MMAL_COMPONENT_T *cp_cameras[MAX_CAMERAS];
static struct cameras_config {
    _Bool is_used;
    int32_t width, height;
    int32_t max_width, max_height;
} cameras_config[MAX_CAMERAS];

static MMAL_COMPONENT_T *cp_splitters[MAX_CAMERAS];
static struct splitters_config {
    int next_output_idx;
} splitters_config[MAX_CAMERAS];

static MMAL_COMPONENT_T *cp_isps[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];
static struct isps_config {
    int32_t width, height;
    MMAL_FOURCC_T encoding;
    _Bool is_zero_copy_rendering;
} isps_config[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];

static MMAL_COMPONENT_T *cp_renders[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];
static struct renders_config {
    MMAL_DISPLAYREGION_T region;
} renders_config[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];

static MMAL_CONNECTION_T *conn_camera_splitters[MAX_CAMERAS];
static MMAL_CONNECTION_T *conn_splitters_isps[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];
static MMAL_CONNECTION_T *conn_isps_renders[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];

static struct callback_context *ctxs[MAX_CAMERAS][NUM_SPLITTER_OUTPUTS];

static MMAL_STATUS_T config_port(MMAL_PORT_T *port, const MMAL_FOURCC_T encoding,
                                 const int width, const int height)
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

int priv_rpigrafx_mmal_init()
{
    int i, j;
    int ret = 0;
    MMAL_STATUS_T status;

    if (called.mmal != 0)
        goto end;

    for (i = 0; i < MAX_CAMERAS; i ++) {
        cp_cameras[i] = NULL;
        cameras_config[i].is_used = 0;

        cp_splitters[i] = NULL;
        splitters_config[i].next_output_idx = 0;
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
            cameras_config[i].max_width  = camera_info.cameras[i].max_width;
            cameras_config[i].max_height = camera_info.cameras[i].max_height;
        }
        for (; i < MAX_CAMERAS; i ++) {
            cameras_config[i].max_width  = 0;
            cameras_config[i].max_height = 0;
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
    called.mmal ++;

    return ret;
}

int priv_rpigrafx_mmal_finalize()
{
    int i, j;
    int ret = 0;

    if (called.mmal != 1)
        goto skip;

    for (i = 0; i < MAX_CAMERAS; i ++) {
        cp_cameras[i] = cp_splitters[i] = NULL;
        for (j = 0; j < NUM_SPLITTER_OUTPUTS; j ++)
            cp_isps[i][j] = NULL;
        cameras_config[i].width  = -1;
        cameras_config[i].height = -1;
        cameras_config[i].max_width  = -1;
        cameras_config[i].max_height = -1;
        splitters_config[i].next_output_idx = 0;
    }

skip:
    called.mmal --;
    return ret;
}

static void callback_control(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *header)
{
    MMAL_PARAM_UNUSED(port);
    mmal_buffer_header_release(header);
}

static void callback_conn(MMAL_CONNECTION_T *conn)
{
    struct callback_context *ctx = (struct callback_context*) conn->user_data;
    vcos_semaphore_post(&ctx->sem);
}

int rpigrafx_config_camera_frame(const int32_t camera_number,
                                 const int32_t width, const int32_t height,
                                 const MMAL_FOURCC_T encoding,
                                 const _Bool is_zero_copy_rendering,
                                 rpigrafx_frame_config_t *fcp)
{
    int32_t max_width, max_height;
    int idx;
    struct callback_context *ctx = NULL;
    VCOS_STATUS_T status_vcos;
    int ret = 0;

    if (camera_number >= num_cameras) {
        print_error("camera_number(%d) exceeds num_cameras(%d)",
                    camera_number, num_cameras);
        ret = 1;
        goto end;
    }
    max_width  = cameras_config[camera_number].max_width;
    max_height = cameras_config[camera_number].max_height;
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

    if (encoding != MMAL_ENCODING_RGBA) {
        print_error("Only RGBA is supported for now");
        ret = 1;
        goto end;
    }

    /*
     * Only set use flag here.
     * cameras_config[camera_number].{width,height}
     * will be set on rpigrafx_finish_config.
     */
    cameras_config[camera_number].is_used = !0;

    if (splitters_config[camera_number].next_output_idx == NUM_SPLITTER_OUTPUTS - 1) {
        print_error("Too many splitter clients(%d) of camera %d",
                    splitters_config[camera_number].next_output_idx,
                    camera_number);
        ret = 1;
        goto end;
    }
    idx = splitters_config[camera_number].next_output_idx ++;

    isps_config[camera_number][idx].width  = width;
    isps_config[camera_number][idx].height = height;
    isps_config[camera_number][idx].encoding = encoding;
    isps_config[camera_number][idx].is_zero_copy_rendering = is_zero_copy_rendering;

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        print_error("Failed to allocate context");
        ret = 1;
        goto end;
    }
    ctx->status = MMAL_SUCCESS;
    ctx->header = NULL;
    /* Note: It seems that the name can be duplicated. It can even be NULL. */
    status_vcos = vcos_semaphore_create(&ctx->sem, "conn callback sem", 0);
    if (status_vcos != VCOS_SUCCESS) {
        print_error("Failed to create semaphore: 0x%08x", status_vcos);
        ret = 1;
        goto end;
    }
    ctxs[camera_number][idx] = ctx;

    fcp->camera_number = camera_number;
    fcp->splitter_output_port_index = idx;
    fcp->is_zero_copy_rendering = is_zero_copy_rendering;
    fcp->ctx = ctx;

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
    int ret = 0;

    memcpy(&renders_config[fcp->camera_number][fcp->splitter_output_port_index].region, &region, sizeof(region));

    return ret;
}

int rpigrafx_finish_config()
{
    int i, j;
    MMAL_STATUS_T status;
    int ret = 0;

    for (i = 0; i < num_cameras; i ++) {
        int len;
        /* Maximum width/height of the requested frames. */
        int32_t max_width, max_height;

        if (!cameras_config[i].is_used)
            continue;

        len = splitters_config[i].next_output_idx;

        max_width = max_height = 0;
        for (j = 0; j < len; j ++) {
            max_width  = MMAL_MAX(max_width,  isps_config[i][j].width);
            max_height = MMAL_MAX(max_height, isps_config[i][j].height);
        }

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
        {
            MMAL_PORT_T *output = mmal_util_get_port(cp_cameras[i],
                                                     MMAL_PORT_TYPE_OUTPUT, 0);

            if (output == NULL) {
                print_error("Getting output port of camera %d failed", i);
                ret = 1;
                goto end;
            }

            status = config_port(output, MMAL_ENCODING_RGBA, max_width, max_height);
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

        status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_SPLITTER, &cp_splitters[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Creating splitter component of camera %d failed: 0x%08x",
                        i, status);
            ret = 1;
            goto end;
        }
        {
            MMAL_PORT_T *control = mmal_util_get_port(cp_splitters[i],
                                                      MMAL_PORT_TYPE_CONTROL, 0);

            if (control == NULL) {
                print_error("Getting control port of splitter %d failed", i);
                ret = 1;
                goto end;
            }

            status = mmal_port_enable(control, callback_control);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling control port of splitter %d failed: 0x%08x",
                            i, status);
                ret = 1;
                goto end;
            }
        }
        {
            MMAL_PORT_T *input = mmal_util_get_port(cp_splitters[i],
                                                    MMAL_PORT_TYPE_INPUT, 0);

            if (input == NULL) {
                print_error("Getting input port of splitter %d failed", i);
                ret = 1;
                goto end;
            }

            status = config_port(input, MMAL_ENCODING_OPAQUE, max_width, max_height);
            if (status != MMAL_SUCCESS) {
                print_error("Setting format of " \
                            "splitter %d input failed: 0x%08x", i, status);
                ret = 1;
                goto end;
            }

            status = mmal_port_parameter_set_boolean(input,
                                                MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
            if (status != MMAL_SUCCESS) {
                print_error("Setting zero-copy on " \
                            "splitter %d input failed: 0x%08x", i, status);
                ret = 1;
                goto end;
            }
        }
        for (j = 0; j < len; j ++) {
            MMAL_PORT_T *output = mmal_util_get_port(cp_splitters[i],
                                                     MMAL_PORT_TYPE_OUTPUT, j);

            if (output == NULL) {
                print_error("Getting output port of splitter %d,%d failed", i, j);
                ret = 1;
                goto end;
            }

            status = config_port(output, MMAL_ENCODING_RGBA, max_width, max_height);
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
        status = mmal_component_enable(cp_splitters[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling splitter component of " \
                        "camera %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }

        for (j = 0; j < len; j ++) {
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

                if (input == NULL) {
                    print_error("Getting input port of isp %d,%d failed", i, j);
                    ret = 1;
                    goto end;
                }

                status = config_port(input, MMAL_ENCODING_RGBA, max_width, max_height);
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

                status = config_port(output,
                                     isps_config[i][j].encoding,
                                     isps_config[i][j].width,
                                     isps_config[i][j].height);
                if (status != MMAL_SUCCESS) {
                    print_error("Setting format of " \
                                "isp %d output %d failed: 0x%08x", i, j, status);
                    ret = 1;
                    goto end;
                }

                status = mmal_port_parameter_set_boolean(output,
                                                    MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
                if (status != MMAL_SUCCESS) {
                    print_error("Setting zero-copy on " \
                                "isp %d output %d failed: 0x%08x", i, j, status);
                    ret = 1;
                    goto end;
                }
            }
            status = mmal_component_enable(cp_isps[i][j]);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling isp component %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }
        for (j = 0; j < len; j ++) {
            status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &cp_renders[i][j]);
            if (status != MMAL_SUCCESS) {
                print_error("Creating render component %d,%d failed: 0x%08x", i, j, status);
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

                status = config_port(input, MMAL_ENCODING_RGBA, max_width, max_height);
                if (status != MMAL_SUCCESS) {
                    print_error("Setting format of " \
                                "render %d input %d failed: 0x%08x", i, j, status);
                    ret = 1;
                    goto end;
                }

                status = mmal_util_set_display_region(input, &renders_config[i][j].region);
                if (status != MMAL_SUCCESS) {
                    print_error("Setting region of " \
                                "render %d input %d failed: 0x%08x", i, j, status);
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
            status = mmal_component_enable(cp_renders[i][j]);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling render component %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }

        status = mmal_connection_create(&conn_camera_splitters[i],
                                        cp_cameras[i]->output[0],
                                        cp_splitters[i]->input[0],
                                        MMAL_CONNECTION_FLAG_TUNNELLING);
        if (status != MMAL_SUCCESS) {
            print_error("Connecting " \
                        "camera and splitter ports %d failed: 0x%08x", i, status);
            ret = 1;
            goto end;
        }
        for (j = 0; j < len; j ++) {
            status = mmal_connection_create(&conn_splitters_isps[i][j],
                                            cp_splitters[i]->output[j],
                                            cp_isps[i][j]->input[0],
                                            MMAL_CONNECTION_FLAG_TUNNELLING);
            if (status != MMAL_SUCCESS) {
                print_error("Connecting " \
                            "splitter and isp ports %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
            status = mmal_connection_create(&conn_isps_renders[i][j],
                                            cp_isps[i][j]->output[0],
                                            cp_renders[i][j]->input[0],
                                            0);
            if (status != MMAL_SUCCESS) {
                print_error("Connecting " \
                            "isp and render ports %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }

        for (j = 0; j < len; j ++) {
            conn_isps_renders[i][j]->user_data = (void*) ctxs[i][j];
            conn_isps_renders[i][j]->callback = callback_conn;
            status = mmal_connection_enable(conn_isps_renders[i][j]);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling connection between " \
                            "splitter and isp %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
            status = mmal_connection_enable(conn_splitters_isps[i][j]);
            if (status != MMAL_SUCCESS) {
                print_error("Enabling connection between " \
                            "splitter and isp %d,%d failed: 0x%08x", i, j, status);
                ret = 1;
                goto end;
            }
        }
        status = mmal_connection_enable(conn_camera_splitters[i]);
        if (status != MMAL_SUCCESS) {
            print_error("Enabling connection between " \
                        "camera and splitter %d,%d failed: 0x%08x", i, j, status);
            ret = 1;
            goto end;
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
    }

end:
    return ret;
}

int rpigrafx_capture_next_frame(rpigrafx_frame_config_t *fcp)
{
    struct callback_context *ctx = fcp->ctx;
    int ret = 0;
    MMAL_BUFFER_HEADER_T *header = NULL;
    MMAL_CONNECTION_T *conn = conn_isps_renders[fcp->camera_number][fcp->splitter_output_port_index];

    while ((header = mmal_queue_get(conn->pool->queue)) != NULL) {
        mmal_port_send_buffer(conn->out, header);
    }
    ctx->header = mmal_queue_wait(conn->queue);

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

    status = mmal_port_send_buffer(conn_isps_renders[fcp->camera_number][fcp->splitter_output_port_index]->in, fcp->ctx->header);
    if (status != MMAL_SUCCESS) {
        print_error("Sending header to render failed: 0x%08x", status);
        goto end;
    }

end:
    return ret;
}
