#pragma once

// World<->screen transform with pan and zoom. Direct port of game/camera.py.
class Camera {
public:
    Camera(int view_w, int view_h, double world_w, double world_h);

    void resize(int w, int h);
    void center_on(double wx, double wy);
    void pan(double dx, double dy);
    void clamp();

    void world_to_screen(double wx, double wy, int& sx, int& sy) const;
    void screen_to_world(int sx, int sy, double& wx, double& wy) const;

    // (x, y, w, h) of the world-space rect currently visible.
    struct Rect { double x, y, w, h; };
    Rect visible_rect() const;

    double x = 0.0, y = 0.0;
    double zoom = 1.0;
    int vw, vh;
    double world_w, world_h;
};
