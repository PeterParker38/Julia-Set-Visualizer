#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include <string>
#include <algorithm>
using namespace std;

// Force NVIDIA GPU on Optimus laptops instead of integrated Intel graphics
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) unsigned long AmdPowerXpressRequestHighPerformance = 1;
}

// Passes each vertex's NDC position (-1 to 1) straight through to the fragment shader
const char* vertSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vPos;
void main() {
    vPos = aPos;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Runs once per pixel on the GPU — contains all Julia set math and coloring
const char* fragSrc = R"(
#version 330 core
in  vec2 vPos;
out vec4 fragColor;

uniform vec2  u_c;
uniform vec2  u_offset;
uniform float u_zoom;
uniform float u_aspect;
uniform float u_time;
uniform int u_maxIter;

// Cosine palette: maps a float t in [0,1] to a RGB color pallete
// Formula: color = a + b * cos(2pi * (c*t + d)), each vec3 controls R,G,B independently
vec3 colorPalette(float t) {
    vec3 a = vec3(0.4,  0.0,  0.5);
    vec3 b = vec3(0.5,  0.4,  0.5);
    vec3 c = vec3(1.0,  1.0,  0.5);
    vec3 d = vec3(0.0,  0.25, 0.75);
    return a + b * cos(6.28318 * (c * t + d));
}

void main() {
    // Map this pixel's NDC position to a point in the complex plane, applying aspect, zoom, and pan
    vec2 z = vec2(vPos.x * u_aspect, vPos.y) / u_zoom + u_offset;

    int iter    = 0;
    int maxIter = u_maxIter;

    // Iterate z = z^2 + c; count steps before |z| exceeds 2 (escape radius)
    // dot(z,z) is |z|^2 — avoids a sqrt compared to length(z) > 2.0
    for (int i = 0; i < maxIter; i++) {
        if (dot(z, z) > 4.0) break;
        z = vec2(
            z.x * z.x - z.y * z.y + u_c.x,
            2.0 * z.x * z.y       + u_c.y
        );
        iter++;
    }

    if (iter == maxIter) {
        // Point is inside the set — draw black so colored edges pop
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
    
    else {
        // Smooth escape-time: removes harsh integer color bands
        // Uses log(|z|) at escape to compute a fractional iteration count
        float log_zn = log(dot(z, z)) * 0.5;
        float nu     = log(log_zn) / log(2.0);
        float smoothT = float(iter) + 1.0 - nu;

        // Normalize to [0,1] and add slow time offset for color animation
        // fract() wraps t back into [0,1] as it grows over time
        float t = smoothT / float(maxIter) + u_time * 0.04;
        t = fract(t);

        vec3 col = colorPalette(t);
        fragColor = vec4(col, 1.0);
    }
}
)";

// Global zoom so the scroll callback can modify it (GLFW callbacks are plain C function pointers)
float zoom = 0.65f;
float panX = 0.0f;
float panY = 0.0f;

// Zoom in/out by 10% per scroll tick — multiplicative so it feels consistent at any zoom level
void scrollCallback(GLFWwindow* /*window*/, double /*xoffset*/, double yoffset) {
    zoom *= (yoffset > 0.0) ? 1.1f : 0.9f;
}

// Compile a GLSL shader from source; print error log if compilation fails
GLuint compileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        cerr << "[Shader Error]\n" << log << endl;
    }
    return shader;
}

// Compile both shaders and link them into one GPU program; delete shader objects once linked
GLuint createProgram(const char* vSrc, const char* fSrc) {
    GLuint vert = compileShader(GL_VERTEX_SHADER, vSrc);
    GLuint frag = compileShader(GL_FRAGMENT_SHADER, fSrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

int main() {

    if (!glfwInit()) { std::cerr << "GLFW init failed\n"; return -1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    const int WIDTH = 1280;
    const int HEIGHT = 720;

    GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT,
        "Julia Set | SPACE = lock c | Scroll = zoom | WASD = pan",
        nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetScrollCallback(window, scrollCallback);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { std::cerr << "GLEW init failed\n"; return -1; }

    // Two triangles covering NDC space [-1,1] — gives the GPU a surface to run the fragment shader on
    float verts[] = {
        -1.f,-1.f,  1.f,-1.f,  1.f, 1.f,
        -1.f,-1.f,  1.f, 1.f, -1.f, 1.f
    };

    // VBO holds raw vertex bytes in VRAM; VAO records how to interpret them
    // VAO must be bound first so it captures the subsequent VBO setup calls
    GLuint VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    GLuint prog = createProgram(vertSrc, fragSrc);
    glUseProgram(prog);

    // Cache uniform locations once — avoids a string lookup inside the render loop
    GLint loc_c = glGetUniformLocation(prog, "u_c");
    GLint loc_offset = glGetUniformLocation(prog, "u_offset");
    GLint loc_zoom = glGetUniformLocation(prog, "u_zoom");
    GLint loc_aspect = glGetUniformLocation(prog, "u_aspect");
    GLint loc_time = glGetUniformLocation(prog, "u_time");
    GLint loc_maxIter = glGetUniformLocation(prog, "u_maxIter");

    glUniform1f(loc_aspect, (float)WIDTH / (float)HEIGHT);

    float sensitivity = 0.5f;
    bool  cLocked = false;
    bool  spaceWasDown = false;
    float cx = 0.0f, cy = 0.0f;

    double startTime = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // Edge detection: toggle lock only on the frame Space transitions from up to down
        bool spaceIsDown = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (spaceIsDown && !spaceWasDown)
            cLocked = !cLocked;
        spaceWasDown = spaceIsDown;

        // Pan speed scales inversely with zoom so movement feels consistent at all depths
        float panSpeed = 0.015f / zoom;
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) panY += panSpeed;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) panY -= panSpeed;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) panX -= panSpeed;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) panX += panSpeed;

        // Map cursor position to complex range [-1, 1] (sensitivity 0.5 halves the raw [-2,2] range)
        // Y is negated to flip screen-down to math-up
        if (!cLocked) {
            double mx, my;
            glfwGetCursorPos(window, &mx, &my);
            cx = (float)(mx / WIDTH * 4.0 - 2.0) * sensitivity;
            cy = -(float)(my / HEIGHT * 4.0 - 2.0) * sensitivity;
        }

        float elapsed = (float)(glfwGetTime() - startTime);

        glUniform2f(loc_c, cx, cy);
        glUniform2f(loc_offset, panX, panY);
        glUniform1f(loc_zoom, zoom);
        glUniform1f(loc_time, elapsed);

        // Scale iteration limit with zoom depth — more detail when zoomed in, faster at low zoom
        int maxIter = (int)(256.0f * log2f(zoom + 2.0f));
        maxIter = clamp(maxIter, 256, 2048);
        glUniform1i(loc_maxIter, maxIter);

        glClear(GL_COLOR_BUFFER_BIT);
        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glfwSwapBuffers(window);
    }

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteProgram(prog);
    glfwTerminate();
    return 0;
}
