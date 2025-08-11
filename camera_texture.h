#ifndef CAMERA_TEXTURE_H
#define CAMERA_TEXTURE_H

#include <flutter_embedder.h>
#include <GL/gl.h>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>

class CameraTexture {
public:
    explicit CameraTexture(FlutterEngine engine);
    ~CameraTexture();
    
    // Инициализация текстуры и регистрация во Flutter
    bool Initialize();
    
    // Запуск/остановка захвата видео
    bool Start();
    void Stop();
    
    // Получить ID текстуры для Flutter
    int64_t GetTextureId() const { return texture_id_; }
    
    // Получить информацию о текстуре для Flutter
    bool GetTextureInfo(FlutterOpenGLTexture* texture);
    
private:
    // Цикл захвата видео
    void CaptureLoop();
    
    // Обновление OpenGL текстуры
    void UpdateTexture(const uint8_t* rgb_data);
    
    // Конвертация YUV420 в RGB
    void ConvertYUV420ToRGB(const uint8_t* yuv_data, uint8_t* rgb_data);
    
    FlutterEngine engine_;
    int64_t texture_id_;
    GLuint gl_texture_;
    
    FILE* camera_pipe_;
    std::atomic<bool> is_running_;
    std::thread capture_thread_;
    
    int frame_width_;
    int frame_height_;
    std::vector<uint8_t> frame_buffer_;
    std::vector<uint8_t> rgb_buffer_;
};

#endif // CAMERA_TEXTURE_H