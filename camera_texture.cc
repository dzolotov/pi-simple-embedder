#include "camera_texture.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <iostream>

CameraTexture::CameraTexture(FlutterEngine engine) 
    : engine_(engine), texture_id_(0), gl_texture_(0), 
      camera_pipe_(nullptr), is_running_(false),
      frame_width_(640), frame_height_(480) {
    
    // Размер буфера для YUV420: width * height * 1.5
    frame_buffer_.resize(frame_width_ * frame_height_ * 3 / 2);
    rgb_buffer_.resize(frame_width_ * frame_height_ * 3);
}

CameraTexture::~CameraTexture() {
    Stop();
    if (gl_texture_ != 0) {
        glDeleteTextures(1, &gl_texture_);
    }
}

bool CameraTexture::Initialize() {
    // Создаем OpenGL текстуру
    glGenTextures(1, &gl_texture_);
    glBindTexture(GL_TEXTURE_2D, gl_texture_);
    
    // Параметры текстуры
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Инициализируем пустую текстуру
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 
                 frame_width_, frame_height_, 0, 
                 GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    
    // Регистрируем текстуру во Flutter
    FlutterOpenGLTexture fl_texture = {};
    fl_texture.target = GL_TEXTURE_2D;
    fl_texture.name = gl_texture_;
    fl_texture.format = GL_RGB8;
    fl_texture.user_data = this;
    fl_texture.destruction_callback = nullptr;
    fl_texture.width = frame_width_;
    fl_texture.height = frame_height_;
    
    FlutterEngineRegisterExternalTexture(engine_, &texture_id_);
    
    std::cout << "Camera texture initialized with ID: " << texture_id_ 
              << ", GL texture: " << gl_texture_ << std::endl;
    
    return true;
}

bool CameraTexture::Start() {
    if (is_running_) {
        return true;
    }
    
    // Запускаем rpicam-vid для захвата видео в формате YUV420
    // -t 0 - бесконечный захват
    // --codec yuv420 - формат вывода YUV420
    // --inline - включить заголовки в поток
    // -o - - вывод в stdout
    std::string command = "rpicam-vid -t 0 --width " + std::to_string(frame_width_) + 
                         " --height " + std::to_string(frame_height_) + 
                         " --framerate 30 --codec yuv420 --inline -o -";
    
    camera_pipe_ = popen(command.c_str(), "r");
    if (!camera_pipe_) {
        std::cerr << "Failed to start camera stream" << std::endl;
        return false;
    }
    
    is_running_ = true;
    
    // Запускаем поток для чтения кадров
    capture_thread_ = std::thread([this]() {
        CaptureLoop();
    });
    
    std::cout << "Camera stream started" << std::endl;
    return true;
}

void CameraTexture::Stop() {
    if (!is_running_) {
        return;
    }
    
    is_running_ = false;
    
    if (camera_pipe_) {
        pclose(camera_pipe_);
        camera_pipe_ = nullptr;
    }
    
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    
    std::cout << "Camera stream stopped" << std::endl;
}

void CameraTexture::CaptureLoop() {
    size_t yuv_frame_size = frame_width_ * frame_height_ * 3 / 2;
    
    while (is_running_ && camera_pipe_) {
        // Читаем YUV420 кадр
        size_t bytes_read = fread(frame_buffer_.data(), 1, yuv_frame_size, camera_pipe_);
        
        if (bytes_read == yuv_frame_size) {
            // Конвертируем YUV420 в RGB
            ConvertYUV420ToRGB(frame_buffer_.data(), rgb_buffer_.data());
            
            // Обновляем текстуру
            UpdateTexture(rgb_buffer_.data());
            
            // Уведомляем Flutter о новом кадре
            FlutterEngineMarkExternalTextureFrameAvailable(engine_, texture_id_);
        } else if (bytes_read == 0) {
            // Конец потока или ошибка
            std::cerr << "Camera stream ended or error occurred" << std::endl;
            break;
        }
        
        // Небольшая задержка для контроля FPS (примерно 30 FPS)
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

void CameraTexture::UpdateTexture(const uint8_t* rgb_data) {
    glBindTexture(GL_TEXTURE_2D, gl_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    frame_width_, frame_height_,
                    GL_RGB, GL_UNSIGNED_BYTE, rgb_data);
}

void CameraTexture::ConvertYUV420ToRGB(const uint8_t* yuv_data, uint8_t* rgb_data) {
    const uint8_t* y_plane = yuv_data;
    const uint8_t* u_plane = yuv_data + frame_width_ * frame_height_;
    const uint8_t* v_plane = u_plane + (frame_width_ * frame_height_) / 4;
    
    for (int j = 0; j < frame_height_; j++) {
        for (int i = 0; i < frame_width_; i++) {
            int y_index = j * frame_width_ + i;
            int uv_index = (j / 2) * (frame_width_ / 2) + (i / 2);
            
            int y = y_plane[y_index];
            int u = u_plane[uv_index];
            int v = v_plane[uv_index];
            
            // YUV to RGB conversion
            int r = y + 1.402 * (v - 128);
            int g = y - 0.344 * (u - 128) - 0.714 * (v - 128);
            int b = y + 1.772 * (u - 128);
            
            // Clamp values
            r = (r < 0) ? 0 : (r > 255) ? 255 : r;
            g = (g < 0) ? 0 : (g > 255) ? 255 : g;
            b = (b < 0) ? 0 : (b > 255) ? 255 : b;
            
            int rgb_index = y_index * 3;
            rgb_data[rgb_index] = r;
            rgb_data[rgb_index + 1] = g;
            rgb_data[rgb_index + 2] = b;
        }
    }
}

bool CameraTexture::GetTextureInfo(FlutterOpenGLTexture* texture) {
    if (!texture || gl_texture_ == 0) {
        return false;
    }
    
    texture->target = GL_TEXTURE_2D;
    texture->name = gl_texture_;
    texture->format = GL_RGB8;
    texture->user_data = this;
    texture->destruction_callback = nullptr;
    texture->width = frame_width_;
    texture->height = frame_height_;
    
    return true;
}