// graphics demo showing Lobster's built-in loading of (animated) .iqm models

include "vec.lobster"
include "color.lobster"
include "3dhelpers.lobster"

vrmode := vr_init()

fatal(gl_window(if vrmode: "VR Test" else: "No VR device", 1280, 640, false, vrmode))

// Make a quick sample scene out of some trees.

def model_tree(numb, branchl, narrowf, leafs):
    if numb:
        mg_scalevec(xyz_1 * [ 1, 1, branchl ]):
            mg_translate(xyz_z):
                mg_tapered_cylinder(1, narrowf)
        mg_translate(xyz_z * branchl * 2):
            axis := sincos(rnd(360))
            branches := rnd(3) + 1
            for(branches) i:
                mg_rotate(xyz_z, 360 * i / branches):
                    mg_rotate(axis, 12 * branches):
                        mg_scalevec(xyz_1 * narrowf):
                            mg_sphere()
                            model_tree(numb - 1, branchl, narrowf, leafs)
    else:
        mg_fill(2):
            mg_scalevec(xyz_1 * leafs):
                mg_sphere()

model_tree(10, 1.5, 0.75, 12)
tree := mg_polygonize(50, [[ 0.7, 0.6, 0.5 ], [ 0.6, 1.0, 0.6 ]])

mg_sphere()
sphere := mg_polygonize(3, [ color_red ])

camera_position = xyz { 0.0, 0.0, 3.0 }
camera_yaw = 135
camera_pitch = 20

balls := []::[xyz_f]

def drawworld():
    gl_clear(color_grey)
    gl_blend(0)

    gl_light(camera_position, [ 64.0, 1.0 ])
    //gl_light(sincos(gl_time() * 20) * 100 + xyz_z * 100)

    gl_setshader("flat")
    for(10) i:
        gl_translate sincos(i * 72) * 3:
            gl_scale(0.3):
                gl_rendermesh(tree)

    for(balls) b:
        gl_translate b[0]:
            gl_scale(0.1):
                gl_rotate_z(sincos(gl_time() * 30)):
                    gl_rendermesh(sphere)

    gl_setshader("color")
    gl_debug_grid([ 20, 20, 0 ], xyz_1, 0.005)

while gl_frame():
    if gl_wentdown("escape"): return

    if !vrmode: gl_cursor(0)
    camera_FPS_update("w", "a", "s", "d", 10, 4, true)

    znear := 0.1
    zfar := 1000
    if vrmode:
        for(2) e:
            vr_starteye(e, znear, zfar)
            camera_set_z_up_worldspace():
                drawworld()
            for(vr_nummotioncontrollers()) mc:
                mcmesh := vr_motioncontrollermesh(mc)
                if mcmesh:
                    gl_color(color_white)
                    vr_motioncontroller(mc):
                        gl_setshader("phong_textured")
                        gl_rendermesh(mcmesh)
        vr_finish()
        for(2) mc:
            if vr_motioncontrollerbutton(mc, "trigger") == 1:
                pos := to_z_up(vr_motioncontrollervec(mc, 3))
                dir := to_z_up(vr_motioncontrollervec(mc, 2))
                balls.push([ pos, -dir ])
        // Render the two eye textures to the non-VR window for feedback:
        gl_clear(color_black)
        gl_blend(0)
        gl_setshader("textured")

        for(2) e:
            gl_setprimitivetexture(0, vr_geteyetex(e))
            gl_translate([ gl_windowsize().x * e / 2.0, gl_windowsize().y ]):
                    gl_rect([ gl_windowsize().x / 2.0, -gl_windowsize().y ])
    else:
        // This is how we'd render normally, with no HMD connected:
        gl_clear(color_dark_grey)
        gl_blend(0)
        gl_perspective(70, znear, zfar)
        camera_FPS_view()
        drawworld()

    if balls.length > 100:
        balls.remove(0, balls.length - 100)
    for(balls) b:
        b[0] += b[1] * gl_deltatime() / 10
