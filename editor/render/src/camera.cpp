#include "render/camera.h"

#include <algorithm>

Camera::Camera(int view_w, int view_h, double world_w_, double world_h_)
    : vw(view_w), vh(view_h), world_w(world_w_), world_h(world_h_) {
}

void Camera::resize(int w, int h) {
    vw = w;
    vh = h;
    clamp();
}

void Camera::center_on(double wx, double wy) {
    x = wx - vw / (2 * zoom);
    y = wy - vh / (2 * zoom);
    clamp();
}

void Camera::pan(double dx, double dy) {
    x += dx / zoom;
    y += dy / zoom;
    clamp();
}

void Camera::clamp() {
    double maxx = std::max(0.0, world_w - vw / zoom);
    double maxy = std::max(0.0, world_h - vh / zoom);
    x = std::min(std::max(x, 0.0), maxx);
    y = std::min(std::max(y, 0.0), maxy);
}

void Camera::world_to_screen(double wx, double wy, int& sx, int& sy) const {
    sx = static_cast<int>((wx - x) * zoom);
    sy = static_cast<int>((wy - y) * zoom);
}

void Camera::screen_to_world(int sx, int sy, double& wx, double& wy) const {
    wx = sx / zoom + x;
    wy = sy / zoom + y;
}

Camera::Rect Camera::visible_rect() const {
    return {x, y, vw / zoom, vh / zoom};
}
