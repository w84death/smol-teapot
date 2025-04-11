#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>
#include <math.h>
#include <furi_hal_resources.h>

/* generated by fbt from .png files in images folder */
#include <p1x_smol_teapot_icons.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define PROJECTION_DISTANCE 110
#define MAX_VERTICES 256
#define MAX_FACES 100
#define FRAME_INTERVAL 100 // Limit frame rate to prevent freezing

typedef struct {
    float x, y, z;
} Vec3f;

typedef struct {
    int v1, v2, v3;
} Face;

typedef struct {
    float m[4][4];
} Matrix4x4;

// Teapot model data
static Vec3f vertices[MAX_VERTICES];
static Face faces[MAX_FACES];
static int vertex_count = 0;
static int face_count = 0;

// Model state
static Vec3f rotation = {0};
static Vec3f position = {0, 0, 30}; // Moved camera farther back (was 15)
static float scale = 5.0f;
static uint32_t last_frame_time = 0; // For frame rate limiting

// Function prototypes
static void init_teapot_model();
static void render_frame(Canvas* canvas);
static void init_identity_matrix(Matrix4x4* m);
static void rotate_x_matrix(Matrix4x4* m, float angle);
static void rotate_y_matrix(Matrix4x4* m, float angle);
static void rotate_z_matrix(Matrix4x4* m, float angle);
static void multiply_matrix_vector(Matrix4x4* m, Vec3f* in, Vec3f* out);
static float dot_product(Vec3f* v1, Vec3f* v2);
static void cross_product(Vec3f* v1, Vec3f* v2, Vec3f* result);
static void subtract_vectors(Vec3f* v1, Vec3f* v2, Vec3f* result);

// App state
typedef struct {
    FuriMutex* mutex;
} TeapotState;

// Input callback function
static void input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

// Draw callback function
static void render_callback(Canvas* canvas, void* ctx) {
    furi_assert(ctx);
    TeapotState* state = ctx;
    
    if(furi_mutex_acquire(state->mutex, 100) != FuriStatusOk) return;
    
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    
    // Render our 3D scene
    render_frame(canvas);

    // Display controls
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 10, "Up/Dn/L/R: Rotate");
    canvas_draw_str(canvas, 2, 62, "Smol Teapot 3D");
    
    furi_mutex_release(state->mutex);
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

// Initialize a simple teapot model
static void init_teapot_model() {
    // For a simple demo, let's create a teapot-like object
    // A teapot can be approximated with a few connected boxes
    // Base/body of the teapot
    vertices[0] = (Vec3f){-1.0f, 0.0f, -1.0f};
    vertices[1] = (Vec3f){1.0f, 0.0f, -1.0f};
    vertices[2] = (Vec3f){1.0f, 0.0f, 1.0f};
    vertices[3] = (Vec3f){-1.0f, 0.0f, 1.0f};
    vertices[4] = (Vec3f){-1.0f, 1.0f, -1.0f};
    vertices[5] = (Vec3f){1.0f, 1.0f, -1.0f};
    vertices[6] = (Vec3f){1.0f, 1.0f, 1.0f};
    vertices[7] = (Vec3f){-1.0f, 1.0f, 1.0f};
    
    // Spout
    vertices[8] = (Vec3f){1.0f, 0.5f, 0.0f};
    vertices[9] = (Vec3f){2.0f, 0.5f, 0.0f};
    vertices[10] = (Vec3f){2.0f, 0.8f, 0.0f};
    vertices[11] = (Vec3f){1.0f, 0.8f, 0.0f};
    
    // Handle
    vertices[12] = (Vec3f){-1.0f, 0.5f, 0.0f};
    vertices[13] = (Vec3f){-2.0f, 0.5f, 0.0f};
    vertices[14] = (Vec3f){-2.0f, 0.8f, 0.0f};
    vertices[15] = (Vec3f){-1.0f, 0.8f, 0.0f};
    
    // Lid
    vertices[16] = (Vec3f){-0.5f, 1.0f, -0.5f};
    vertices[17] = (Vec3f){0.5f, 1.0f, -0.5f};
    vertices[18] = (Vec3f){0.5f, 1.0f, 0.5f};
    vertices[19] = (Vec3f){-0.5f, 1.0f, 0.5f};
    vertices[20] = (Vec3f){-0.5f, 1.3f, -0.5f};
    vertices[21] = (Vec3f){0.5f, 1.3f, -0.5f};
    vertices[22] = (Vec3f){0.5f, 1.3f, 0.5f};
    vertices[23] = (Vec3f){-0.5f, 1.3f, 0.5f};
    
    // Lid knob
    vertices[24] = (Vec3f){-0.1f, 1.3f, -0.1f};
    vertices[25] = (Vec3f){0.1f, 1.3f, -0.1f};
    vertices[26] = (Vec3f){0.1f, 1.3f, 0.1f};
    vertices[27] = (Vec3f){-0.1f, 1.3f, 0.1f};
    vertices[28] = (Vec3f){-0.1f, 1.6f, -0.1f};
    vertices[29] = (Vec3f){0.1f, 1.6f, -0.1f};
    vertices[30] = (Vec3f){0.1f, 1.6f, 0.1f};
    vertices[31] = (Vec3f){-0.1f, 1.6f, 0.1f};
    
    vertex_count = 32;
    
    // Define faces (triangles)
    // Body bottom
    faces[0] = (Face){0, 1, 2};
    faces[1] = (Face){0, 2, 3};
    
    // Body sides
    faces[2] = (Face){0, 4, 1};
    faces[3] = (Face){1, 4, 5};
    faces[4] = (Face){1, 5, 2};
    faces[5] = (Face){2, 5, 6};
    faces[6] = (Face){2, 6, 3};
    faces[7] = (Face){3, 6, 7};
    faces[8] = (Face){3, 7, 0};
    faces[9] = (Face){0, 7, 4};
    
    // Body top
    faces[10] = (Face){4, 7, 5};
    faces[11] = (Face){5, 7, 6};
    
    // Spout
    faces[12] = (Face){8, 9, 10};
    faces[13] = (Face){8, 10, 11};
    
    // Handle
    faces[14] = (Face){12, 13, 14};
    faces[15] = (Face){12, 14, 15};
    
    // Lid sides
    faces[16] = (Face){16, 20, 17};
    faces[17] = (Face){17, 20, 21};
    faces[18] = (Face){17, 21, 18};
    faces[19] = (Face){18, 21, 22};
    faces[20] = (Face){18, 22, 19};
    faces[21] = (Face){19, 22, 23};
    faces[22] = (Face){19, 23, 16};
    faces[23] = (Face){16, 23, 20};
    
    // Lid top
    faces[24] = (Face){20, 23, 21};
    faces[25] = (Face){21, 23, 22};
    
    // Knob sides
    faces[26] = (Face){24, 28, 25};
    faces[27] = (Face){25, 28, 29};
    faces[28] = (Face){25, 29, 26};
    faces[29] = (Face){26, 29, 30};
    faces[30] = (Face){26, 30, 27};
    faces[31] = (Face){27, 30, 31};
    faces[32] = (Face){27, 31, 24};
    faces[33] = (Face){24, 31, 28};
    
    // Knob top
    faces[34] = (Face){28, 31, 29};
    faces[35] = (Face){29, 31, 30};
    
    face_count = 36;
}

static void render_frame(Canvas* canvas) {
    // Create rotation matrices
    Matrix4x4 rot_x_matrix, rot_y_matrix, rot_z_matrix;
    
    init_identity_matrix(&rot_x_matrix);
    init_identity_matrix(&rot_y_matrix);
    init_identity_matrix(&rot_z_matrix);
    
    rotate_x_matrix(&rot_x_matrix, rotation.x);
    rotate_y_matrix(&rot_y_matrix, rotation.y);
    rotate_z_matrix(&rot_z_matrix, rotation.z);
    
    // Transformed vertices
    Vec3f transformed[MAX_VERTICES];
    
    // Transform and project all vertices
    for(int i = 0; i < vertex_count; i++) {
        // Apply rotation matrices
        Vec3f rotated = vertices[i];
        Vec3f temp;
        
        multiply_matrix_vector(&rot_x_matrix, &rotated, &temp);
        multiply_matrix_vector(&rot_y_matrix, &temp, &rotated);
        multiply_matrix_vector(&rot_z_matrix, &rotated, &temp);
        
        // Apply position
        temp.x = temp.x * scale + position.x;
        temp.y = temp.y * scale + position.y;
        temp.z = temp.z * scale + position.z;
        
        transformed[i] = temp;
    }
    
    // Draw the wireframe with backface culling - skip faces with z < 1 to prevent divide by zero
    for(int i = 0; i < face_count; i++) {
        // Get the face
        Face face = faces[i];
        
        // Skip faces too close to camera
        if(transformed[face.v1].z < 1.0f || 
           transformed[face.v2].z < 1.0f || 
           transformed[face.v3].z < 1.0f) {
            continue;
        }
        
        // Calculate normal using cross product
        Vec3f v1, v2, normal;
        subtract_vectors(&transformed[face.v2], &transformed[face.v1], &v1);
        subtract_vectors(&transformed[face.v3], &transformed[face.v1], &v2);
        cross_product(&v1, &v2, &normal);
        
        // Calculate dot product with camera direction 
        // Assuming camera is looking towards negative z
        Vec3f camera_dir = {0, 0, -1};
        float dot = dot_product(&normal, &camera_dir);
        
        // Backface culling - only render if facing camera
        if(dot < 0) {
            // Project the 3D vertices to 2D screen space
            int x1 = (int)((transformed[face.v1].x * PROJECTION_DISTANCE) / transformed[face.v1].z) + SCREEN_WIDTH/2;
            int y1 = (int)((transformed[face.v1].y * PROJECTION_DISTANCE) / transformed[face.v1].z) + SCREEN_HEIGHT/2;
            int x2 = (int)((transformed[face.v2].x * PROJECTION_DISTANCE) / transformed[face.v2].z) + SCREEN_WIDTH/2;
            int y2 = (int)((transformed[face.v2].y * PROJECTION_DISTANCE) / transformed[face.v2].z) + SCREEN_HEIGHT/2;
            int x3 = (int)((transformed[face.v3].x * PROJECTION_DISTANCE) / transformed[face.v3].z) + SCREEN_WIDTH/2;
            int y3 = (int)((transformed[face.v3].y * PROJECTION_DISTANCE) / transformed[face.v3].z) + SCREEN_HEIGHT/2;
            
            // Check if any part of the triangle is on screen to avoid wasted drawing
            if((x1 < 0 && x2 < 0 && x3 < 0) || 
               (x1 > SCREEN_WIDTH && x2 > SCREEN_WIDTH && x3 > SCREEN_WIDTH) ||
               (y1 < 0 && y2 < 0 && y3 < 0) || 
               (y1 > SCREEN_HEIGHT && y2 > SCREEN_HEIGHT && y3 > SCREEN_HEIGHT)) {
                continue;
            }
            
            // Draw wireframe triangle
            canvas_draw_line(canvas, x1, y1, x2, y2);
            canvas_draw_line(canvas, x2, y2, x3, y3);
            canvas_draw_line(canvas, x3, y3, x1, y1);
        }
    }
}

int32_t p1x_smol_teapot_app(void* p) {
    UNUSED(p);
    FURI_LOG_I("P1X_SMOL_TEAPOT", "3D Teapot renderer starting");
    
    // Create event queue
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    
    // Set up state
    TeapotState* state = malloc(sizeof(TeapotState));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    
    // Set up viewport
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, state);
    view_port_input_callback_set(view_port, input_callback, event_queue);
    
    // Register viewport with GUI
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    
    // Initialize the teapot model
    init_teapot_model();
    
    // Initialize timing
    last_frame_time = furi_get_tick();
    
    // Handle events
    InputEvent event;
    bool running = true;
    
    while(running) {
        // Rate limit updates to prevent freezing
        uint32_t current_time = furi_get_tick();
        bool frame_ready = (current_time - last_frame_time >= FRAME_INTERVAL);
        
        // Process input with timeout
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, frame_ready ? 0 : FRAME_INTERVAL);
        
        if(event_status == FuriStatusOk) {
            if(furi_mutex_acquire(state->mutex, 100) == FuriStatusOk) {
                // Process key presses
                if(event.type == InputTypePress || event.type == InputTypeRepeat) {
                    switch(event.key) {
                        case InputKeyUp:
                            rotation.x += 0.1f;
                            break;
                        case InputKeyDown:
                            rotation.x -= 0.1f;
                            break;
                        case InputKeyLeft:
                            rotation.y -= 0.1f;
                            break;
                        case InputKeyRight:
                            rotation.y += 0.1f;
                            break;
                        case InputKeyOk:
                            rotation.z += 0.1f;
                            break;
                        case InputKeyBack:
                            running = false;
                            break;
                        default:
                            break;
                    }
                }
                
                furi_mutex_release(state->mutex);
            }
        }
        
        // Update display at controlled frame rate
        if(frame_ready) {
            view_port_update(view_port);
            last_frame_time = current_time;
        } else {
            // Give some time back to the system
            furi_delay_ms(5);
        }
    }
    
    // Clean up
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_mutex_free(state->mutex);
    free(state);
    
    return 0;
}
