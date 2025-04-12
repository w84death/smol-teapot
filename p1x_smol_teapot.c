#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>
#include <math.h>
#include <furi_hal_resources.h>

/* generated by fbt from .png files in images folder */
#include <p1x_smol_teapot_icons.h>

/* include triangulated teapot model */
#include "teapot_decimated.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define PROJECTION_DISTANCE 190
#define FRAME_DELAY 33

// Model bounds to find center
#define MODEL_MIN_X -3.0f
#define MODEL_MAX_X 3.0f
#define MODEL_MIN_Y 0.0f
#define MODEL_MAX_Y 3.3f
#define MODEL_MIN_Z -3.0f
#define MODEL_MAX_Z 3.0f

typedef struct {
    float x, y, z;
} Vec3f;

typedef struct {
    float m[4][4];
} Matrix4x4;

// Model state
static Vec3f rotation = {0};
static Vec3f last_rotation = {0}; // Track last rotation state
static Vec3f position = {0, 0, 30};
static float scale = 2.0f;
static bool render_complete = false;
static bool render_needed = true;

// Model center pivot point
static Vec3f model_center = {
    (MODEL_MIN_X + MODEL_MAX_X) / 2.0f,
    (MODEL_MIN_Y + MODEL_MAX_Y) / 2.0f,
    (MODEL_MIN_Z + MODEL_MAX_Z) / 2.0f
};

// Render buffer to avoid direct drawing to screen
typedef struct {
    uint8_t* buffer;
    uint16_t width;
    uint16_t height;
} RenderBuffer;

static RenderBuffer render_buffer = {0};

// App state
typedef struct {
    FuriMutex* mutex;
    uint32_t fps;
    uint32_t polygons_drawn;
    uint32_t frame_count;
    uint32_t last_frame_time;
    bool auto_rotate;    // Flag to enable auto-rotation
    float auto_rotate_speed;  // Speed of auto-rotation
    uint8_t rotate_axis;     // 0=X, 1=Y, 2=Z
    bool show_title_screen; // Flag to show title screen instead of 3D teapot
} TeapotState;

// Function prototypes
static void render_complete_model(TeapotState* state);
static void init_identity_matrix(Matrix4x4* m);
static void rotate_x_matrix(Matrix4x4* m, float angle);
static void rotate_y_matrix(Matrix4x4* m, float angle);
static void rotate_z_matrix(Matrix4x4* m, float angle);
static void multiply_matrix_vector(Matrix4x4* m, Vec3f* in, Vec3f* out);
static float dot_product(Vec3f* v1, Vec3f* v2);
static void cross_product(Vec3f* v1, Vec3f* v2, Vec3f* result);
static void subtract_vectors(Vec3f* v1, Vec3f* v2, Vec3f* result);

// Draw pixel to our buffer
static void buffer_draw_pixel(uint8_t x, uint8_t y) {
    if(x < render_buffer.width && y < render_buffer.height) {
        uint16_t byte_idx = y * (render_buffer.width / 8) + (x / 8);
        uint8_t bit_pos = x % 8;
        render_buffer.buffer[byte_idx] |= (1 << bit_pos);
    }
}

// Draw line to our buffer (Bresenham's line algorithm)
static void buffer_draw_line(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int e2;
    
    while (true) {
        buffer_draw_pixel(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { if (x0 == x1) break; err += dy; x0 += sx; }
        if (e2 <= dx) { if (y0 == y1) break; err += dx; y0 += sy; }
    }
}

// Input callback function
static void input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

// Draw callback function - copy our buffer to screen
static void render_callback(Canvas* canvas, void* ctx) {
    furi_assert(ctx);
    TeapotState* state = ctx;
    
    if(furi_mutex_acquire(state->mutex, 100) != FuriStatusOk) return;
    
    // Only render to screen if we have a buffer ready
    if(render_buffer.buffer != NULL) {
        canvas_draw_xbm(
            canvas, 
            0, 
            0, 
            render_buffer.width, 
            render_buffer.height, 
            render_buffer.buffer);
    }
    
    // Only show stats and controls when not on title screen
    if(!state->show_title_screen) {
        // Always display the controls text
        canvas_set_color(canvas, ColorBlack);
        canvas_set_font(canvas, FontPrimary);
        if(state->auto_rotate) {
            char axis = 'X' + state->rotate_axis; // 'X', 'Y', or 'Z' based on rotate_axis value
            char auto_text[16];
            snprintf(auto_text, sizeof(auto_text), "AUTO-%c Teapot", axis);
            canvas_draw_str(canvas, 2, 62, auto_text);
        } else {
            canvas_draw_str(canvas, 2, 62, "Smol Teapot");
        }
        
        // Display FPS and polygon count in the corner
        char stats_text[24];
        snprintf(stats_text, sizeof(stats_text), "FPS:%lu  POLY:%lu", state->fps, state->polygons_drawn);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 1, 1, 80, 10);  // Background for better visibility
        canvas_set_color(canvas, ColorBlack);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 9, stats_text);
    } else {
        canvas_draw_icon(canvas, 0, 0, &I_title);
        // If we're showing title screen, display actual text on top of our buffer drawing
        canvas_set_font(canvas, FontPrimary);
        canvas_set_color(canvas, ColorWhite);
        
        // Draw "UTAH TEAPOT" text at top center
        canvas_draw_str_aligned(canvas, 64, 4, AlignCenter, AlignTop, "UTAH TEAPOT");
        canvas_set_color(canvas, ColorBlack);
        canvas_set_font(canvas, FontSecondary);

        // Draw "Press any button" at bottom
        canvas_draw_str_aligned(canvas, 64, 56, AlignCenter, AlignCenter, "Press any button");
    }
    
    furi_mutex_release(state->mutex);
}

// Create and initialize render buffer
static void init_render_buffer() {
    render_buffer.width = SCREEN_WIDTH;
    render_buffer.height = SCREEN_HEIGHT;
    size_t buffer_size = (render_buffer.width / 8) * render_buffer.height;
    render_buffer.buffer = malloc(buffer_size);
    if(render_buffer.buffer) {
        memset(render_buffer.buffer, 0, buffer_size);
    }
}

// Clear render buffer
static void clear_render_buffer() {
    if(render_buffer.buffer) {
        size_t buffer_size = (render_buffer.width / 8) * render_buffer.height;
        memset(render_buffer.buffer, 0, buffer_size);
    }
}

// Free render buffer
static void free_render_buffer() {
    if(render_buffer.buffer) {
        free(render_buffer.buffer);
        render_buffer.buffer = NULL;
    }
}

// Simple 3D math functions
static void init_identity_matrix(Matrix4x4* m) {
    for(int i = 0; i < 4; i++) {
        for(int j = 0; j < 4; j++) {
            m->m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
}

static void rotate_x_matrix(Matrix4x4* m, float angle) {
    m->m[1][1] = cosf(angle);
    m->m[1][2] = -sinf(angle);
    m->m[2][1] = sinf(angle);
    m->m[2][2] = cosf(angle);
}

static void rotate_y_matrix(Matrix4x4* m, float angle) {
    m->m[0][0] = cosf(angle);
    m->m[0][2] = sinf(angle);
    m->m[2][0] = -sinf(angle);
    m->m[2][2] = cosf(angle);
}

static void rotate_z_matrix(Matrix4x4* m, float angle) {
    m->m[0][0] = cosf(angle);
    m->m[0][1] = -sinf(angle);
    m->m[1][0] = sinf(angle);
    m->m[1][1] = cosf(angle);
}

static void multiply_matrix_vector(Matrix4x4* m, Vec3f* in, Vec3f* out) {
    out->x = in->x * m->m[0][0] + in->y * m->m[1][0] + in->z * m->m[2][0] + m->m[3][0];
    out->y = in->x * m->m[0][1] + in->y * m->m[1][1] + in->z * m->m[2][1] + m->m[3][1];
    out->z = in->x * m->m[0][2] + in->y * m->m[1][2] + in->z * m->m[2][2] + m->m[3][2];
    float w = in->x * m->m[0][3] + in->y * m->m[1][3] + in->z * m->m[2][3] + m->m[3][3];
    
    if(w != 0.0f) {
        out->x /= w;
        out->y /= w;
        out->z /= w;
    }
}

static float dot_product(Vec3f* v1, Vec3f* v2) {
    return v1->x * v2->x + v1->y * v2->y + v1->z * v2->z;
}

static void cross_product(Vec3f* v1, Vec3f* v2, Vec3f* result) {
    result->x = v1->y * v2->z - v1->z * v2->y;
    result->y = v1->z * v2->x - v1->x * v2->z;
    result->z = v1->x * v2->y - v1->y * v2->x;
}

static void subtract_vectors(Vec3f* v1, Vec3f* v2, Vec3f* result) {
    result->x = v1->x - v2->x;
    result->y = v1->y - v2->y;
    result->z = v1->z - v2->z;
}

// Render title screen to buffer
static void render_title_screen() {
    // Clear buffer before rendering
    clear_render_buffer();
    
    // Draw a teapot shape as a border (simplified outline)
    // Top of teapot
    buffer_draw_line(30, 15, 98, 15); 
    buffer_draw_line(30, 15, 25, 25);
    buffer_draw_line(98, 15, 103, 25);
    
    // Teapot body outline
    buffer_draw_line(25, 25, 20, 40);
    buffer_draw_line(103, 25, 108, 40);
    buffer_draw_line(20, 40, 108, 40);
    
    // Spout
    buffer_draw_line(20, 30, 10, 35);
    buffer_draw_line(10, 35, 20, 40);
    
    // Handle
    buffer_draw_line(108, 30, 118, 32);
    buffer_draw_line(118, 32, 108, 40);
    
    // Draw "UTAH TEAPOT" text at top center
    // (We can't draw text directly to the buffer, we'll use lines to create some basic text)
    // U
    buffer_draw_line(36, 22, 36, 30);
    buffer_draw_line(36, 30, 44, 30);
    buffer_draw_line(44, 30, 44, 22);
    
    // T
    buffer_draw_line(48, 22, 56, 22);
    buffer_draw_line(52, 22, 52, 30);
    
    // A
    buffer_draw_line(58, 30, 61, 22);
    buffer_draw_line(61, 22, 64, 30);
    buffer_draw_line(59, 26, 63, 26);
    
    // H
    buffer_draw_line(66, 22, 66, 30);
    buffer_draw_line(66, 26, 72, 26);
    buffer_draw_line(72, 22, 72, 30);
    
    // Draw "Press any button" at bottom
    // Draw a box with text inside
    buffer_draw_line(25, 48, 103, 48);
    buffer_draw_line(25, 48, 25, 56);
    buffer_draw_line(25, 56, 103, 56);
    buffer_draw_line(103, 48, 103, 56);
    
    // Use some simple lines to indicate text inside (can't draw actual text to buffer)
    buffer_draw_line(35, 52, 95, 52);
    buffer_draw_line(40, 52, 40, 54);
    buffer_draw_line(90, 52, 90, 54);
    buffer_draw_line(35, 54, 95, 54);
}

static void render_complete_model(TeapotState* state) {
    // Clear buffer before new render
    clear_render_buffer();
    
    // Reset polygon count
    state->polygons_drawn = 0;
    
    // Create rotation matrices
    Matrix4x4 rot_x_matrix, rot_y_matrix, rot_z_matrix;
    
    init_identity_matrix(&rot_x_matrix);
    init_identity_matrix(&rot_y_matrix);
    init_identity_matrix(&rot_z_matrix);
    
    rotate_x_matrix(&rot_x_matrix, rotation.x);
    rotate_y_matrix(&rot_y_matrix, rotation.y);
    rotate_z_matrix(&rot_z_matrix, rotation.z);
    
    // Process all triangles
    for(int i = 0; i < TEAPOT_TRIANGLE_COUNT; i++) {
        // Extract the triangle vertices from the array
        Vec3f v1 = {
            teapot_triangles[i * 9 + 0],
            teapot_triangles[i * 9 + 1],
            teapot_triangles[i * 9 + 2]
        };
        
        Vec3f v2 = {
            teapot_triangles[i * 9 + 3],
            teapot_triangles[i * 9 + 4],
            teapot_triangles[i * 9 + 5]
        };
        
        Vec3f v3 = {
            teapot_triangles[i * 9 + 6],
            teapot_triangles[i * 9 + 7],
            teapot_triangles[i * 9 + 8]
        };
        
        // Center each vertex around the model's center point before rotation
        v1.x -= model_center.x;
        v1.y -= model_center.y;
        v1.z -= model_center.z;
        
        v2.x -= model_center.x;
        v2.y -= model_center.y;
        v2.z -= model_center.z;
        
        v3.x -= model_center.x;
        v3.y -= model_center.y;
        v3.z -= model_center.z;
        
        // Transform vertices
        Vec3f tv1, tv2, tv3;
        Vec3f temp1, temp2;
        
        // Apply rotation matrices to v1 - Note: order is important for proper 3D perspective!
        // X and Y rotations first, then Z rotation
        multiply_matrix_vector(&rot_x_matrix, &v1, &temp1);
        multiply_matrix_vector(&rot_y_matrix, &temp1, &temp2);
        multiply_matrix_vector(&rot_z_matrix, &temp2, &temp1);
        tv1.x = temp1.x * scale + position.x;
        tv1.y = temp1.y * scale + position.y;
        tv1.z = temp1.z * scale + position.z;
        
        // Apply rotation matrices to v2
        multiply_matrix_vector(&rot_x_matrix, &v2, &temp1);
        multiply_matrix_vector(&rot_y_matrix, &temp1, &temp2);
        multiply_matrix_vector(&rot_z_matrix, &temp2, &temp1);
        tv2.x = temp1.x * scale + position.x;
        tv2.y = temp1.y * scale + position.y;
        tv2.z = temp1.z * scale + position.z;
        
        // Apply rotation matrices to v3
        multiply_matrix_vector(&rot_x_matrix, &v3, &temp1);
        multiply_matrix_vector(&rot_y_matrix, &temp1, &temp2);
        multiply_matrix_vector(&rot_z_matrix, &temp2, &temp1);
        tv3.x = temp1.x * scale + position.x;
        tv3.y = temp1.y * scale + position.y;
        tv3.z = temp1.z * scale + position.z;
        
        // Skip triangles with vertices too close to camera
        if(tv1.z < 1.0f || tv2.z < 1.0f || tv3.z < 1.0f) {
            continue;
        }
        
        // Calculate normal using cross product for backface culling
        Vec3f line1, line2, normal;
        subtract_vectors(&tv2, &tv1, &line1);
        subtract_vectors(&tv3, &tv1, &line2);
        cross_product(&line1, &line2, &normal);
        
        // Calculate dot product with camera direction (towards negative z)
        Vec3f camera_dir = {0, 0, 1};
        float dot = dot_product(&normal, &camera_dir);
        
        // Only render if facing camera (backface culling)
        if(dot < 0) {
            // Project the vertices to screen space
            int x1 = (int)((tv1.x * PROJECTION_DISTANCE) / tv1.z) + SCREEN_WIDTH/2;
            int y1 = (int)((-tv1.y * PROJECTION_DISTANCE) / tv1.z) + SCREEN_HEIGHT/2;
            int x2 = (int)((tv2.x * PROJECTION_DISTANCE) / tv2.z) + SCREEN_WIDTH/2;
            int y2 = (int)((-tv2.y * PROJECTION_DISTANCE) / tv2.z) + SCREEN_HEIGHT/2;
            int x3 = (int)((tv3.x * PROJECTION_DISTANCE) / tv3.z) + SCREEN_WIDTH/2;
            int y3 = (int)((-tv3.y * PROJECTION_DISTANCE) / tv3.z) + SCREEN_HEIGHT/2;
            
            // Check if any part of triangle is on screen
            if((x1 < 0 && x2 < 0 && x3 < 0) || 
               (x1 >= SCREEN_WIDTH && x2 >= SCREEN_WIDTH && x3 >= SCREEN_WIDTH) ||
               (y1 < 0 && y2 < 0 && y3 < 0) || 
               (y1 >= SCREEN_HEIGHT && y2 >= SCREEN_HEIGHT && y3 >= SCREEN_HEIGHT)) {
                continue;
            }
            
            // Draw wireframe triangle to our buffer
            buffer_draw_line(x1, y1, x2, y2);
            buffer_draw_line(x2, y2, x3, y3);
            buffer_draw_line(x3, y3, x1, y1);
            
            // Increment polygon count
            state->polygons_drawn++;
        }
    }
    
    // Signal that render is complete
    render_complete = true;
    render_needed = false;
    
    // Remember last rotation state
    last_rotation = rotation;
}

int32_t p1x_smol_teapot_app(void* p) {
    UNUSED(p);
    FURI_LOG_I("P1X_SMOL_TEAPOT", "3D Teapot renderer starting");
    
    // Create event queue
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    
    // Set up state
    TeapotState* state = malloc(sizeof(TeapotState));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->fps = 0;
    state->polygons_drawn = 0;
    state->frame_count = 0;
    state->last_frame_time = furi_get_tick();
    state->auto_rotate = false;
    state->auto_rotate_speed = 0.05f;  // Auto-rotation speed
    state->rotate_axis = 0;  // Start with X axis rotation
    state->show_title_screen = true;  // Start with title screen
    
    // Initialize render buffer
    init_render_buffer();
    
    // Set up viewport
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, state);
    view_port_input_callback_set(view_port, input_callback, event_queue);
    
    // Prevent GUI from auto-clearing the screen
    view_port_set_orientation(view_port, ViewPortOrientationHorizontal);
    
    // Register viewport with GUI
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    
    // Initialize values
    render_needed = true;
    render_complete = false;
    
    // Render the title screen first
    render_title_screen();
    view_port_update(view_port);

    // Handle events
    InputEvent event;
    bool running = true;
    uint32_t last_auto_render = furi_get_tick();
    uint32_t last_auto_rotate_update = furi_get_tick();
    
    while(running) {
        // Process input with timeout (non-blocking)
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, 0);
        
        if(event_status == FuriStatusOk) {
            if(furi_mutex_acquire(state->mutex, 100) == FuriStatusOk) {
                // Check if we're in title screen mode
                if(state->show_title_screen) {
                    // Any button press exits title screen and starts the teapot renderer
                    if(event.type == InputTypePress) {
                        state->show_title_screen = false;
                        FURI_LOG_I("P1X_SMOL_TEAPOT", "Exiting title screen");
                        
                        // Initialize the 3D renderer
                        rotation.x = 0;
                        rotation.y = 0;
                        rotation.z = 0;
                        render_needed = true;
                        
                        // Force multiple initial renders with slight rotations to ensure the entire model is drawn
                        for(int i = 0; i < 5; i++) {
                            rotation.y = i * 0.1f; // Slight rotation to ensure different faces are drawn
                            render_complete_model(state);
                            view_port_update(view_port);
                            furi_delay_ms(20); // Short delay between frames
                        }
                        
                        // Reset rotation to initial position
                        rotation.x = 0;
                        rotation.y = 0;
                        rotation.z = 0;
                        render_needed = true;
                        
                        // Final render to ensure model is complete
                        render_complete_model(state);
                        view_port_update(view_port);
                    }
                } else {
                    // Process key presses and long presses for the teapot scene
                    if(event.type == InputTypePress || event.type == InputTypeRepeat) {
                        switch(event.key) {
                            case InputKeyUp:
                                rotation.x += 0.25f;
                                render_needed = true;
                                break;
                            case InputKeyDown:
                                rotation.x -= 0.25f;
                                render_needed = true;
                                break;
                            case InputKeyLeft:
                                rotation.y -= 0.25f;
                                render_needed = true;
                                break;
                            case InputKeyRight:
                                rotation.y += 0.25f;
                                render_needed = true;
                                break;
                            case InputKeyOk:
                                if(state->auto_rotate) {
                                    // In auto-rotate mode, OK cycles through rotation axes
                                    state->rotate_axis = (state->rotate_axis + 1) % 3; // Cycle through 0,1,2
                                    FURI_LOG_I("P1X_SMOL_TEAPOT", "Auto-rotation axis: %c", 'X' + state->rotate_axis);
                                    render_needed = true;
                                } else {
                                    // Normal mode - reset rotation
                                    rotation.x = 0;
                                    rotation.y = 0;
                                    rotation.z = 0;
                                    render_needed = true;
                                }
                                break;
                            case InputKeyBack:
                                running = false;
                                break;
                            default:
                                break;
                        }
                    } else if(event.type == InputTypeLong) {
                        // Long press handling
                        switch(event.key) {
                            case InputKeyOk:
                                // Toggle auto-rotation mode
                                state->auto_rotate = !state->auto_rotate;
                                FURI_LOG_I("P1X_SMOL_TEAPOT", "Auto-rotation: %s", state->auto_rotate ? "ON" : "OFF");
                                render_needed = true;
                                break;
                            default:
                                break;
                        }
                    }
                }
                
                furi_mutex_release(state->mutex);
            }
        }
        
        // Only handle auto-rotation and rendering for teapot scene
        if(!state->show_title_screen) {
            uint32_t current_time = furi_get_tick();
            
            // Update auto-rotation if enabled
            if(state->auto_rotate && (current_time - last_auto_rotate_update >= 30)) { // Update at ~33fps
                if(furi_mutex_acquire(state->mutex, 100) == FuriStatusOk) {
                    // Apply rotation to the selected axis
                    if(state->rotate_axis == 0) {
                        rotation.x += state->auto_rotate_speed;
                    } else if(state->rotate_axis == 1) {
                        rotation.y += state->auto_rotate_speed;
                    } else if(state->rotate_axis == 2) {
                        rotation.z += state->auto_rotate_speed;
                    }
                    render_needed = true;
                    last_auto_rotate_update = current_time;
                    furi_mutex_release(state->mutex);
                }
            }
            
            // Force a periodic re-render even without user input
            // This ensures the model stays fully rendered
            if(current_time - last_auto_render > 1000) {
                render_needed = true;
                last_auto_render = current_time;
            }
            
            // Check if we need to render a new frame
            if(render_needed) {
                // Render in our own buffer
                render_complete_model(state);
                
                // Update frame count for FPS calculation
                state->frame_count++;
                
                // Calculate FPS every second
                uint32_t elapsed_time = current_time - state->last_frame_time;
                
                // Update FPS every second (1000ms)
                if(elapsed_time >= 1000) {
                    if(furi_mutex_acquire(state->mutex, 100) == FuriStatusOk) {
                        state->fps = (state->frame_count * 1000) / elapsed_time;
                        state->frame_count = 0;
                        state->last_frame_time = current_time;
                        furi_mutex_release(state->mutex);
                    }
                }
                
                // Update the display once per full model render
                view_port_update(view_port);
            }
        }
        
        // Simple frame delay
        furi_delay_ms(FRAME_DELAY);
    }
    
    // Clean up
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_mutex_free(state->mutex);
    free_render_buffer();
    free(state);
    
    return 0;
}
