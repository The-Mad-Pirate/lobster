// Copyright 2014 Wouter van Oortmerssen. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "stdafx.h"

#include "vmdata.h"
#include "natreg.h"

#include "glinterface.h"
#include "sdlinterface.h"

using namespace lobster;

Primitive polymode = PRIM_FAN;

IntResourceManagerCompact<Mesh> *meshes = NULL;

map<string, uint> texturecache;

Shader *currentshader = NULL;
Shader *colorshader = NULL;

float3 lasthitsize = float3_0;
float3 lastframehitsize = float3_0;

bool graphics_initialized = false;

void GraphicsShutDown()  // should be safe to call even if it wasn't initialized partially or at all
{
    VRShutDown();
    
    extern void CleanPhysics(); CleanPhysics();
    extern void MeshGenClear(); MeshGenClear();
    extern void CubeGenClear(); CubeGenClear();
    extern void FontCleanup(); FontCleanup();

    if (meshes)
    {
        delete meshes;
        meshes = NULL;
    }

    texturecache.clear();

    ShaderShutDown();
    currentshader = NULL;
    colorshader = NULL;

    SDLSoundClose();
    SDLShutdown();

    // we don't set this to false on most platforms, as currently SDL doesn't like being reinitialized
    #ifdef __ANDROID__
        // FIXME: really only allow this if the app has been killed
        graphics_initialized = false;
    #endif
}

bool GraphicsFrameStart()
{
    extern void CullFonts(); CullFonts();

    bool cb = SDLFrame();

    lastframehitsize = lasthitsize;
    lasthitsize = float3_0;

    OpenGLFrameStart(GetScreenSize());
    Set2DMode(GetScreenSize(), true);

    currentshader = colorshader;

    g_vm->LogFrame();

    return cb;
}

void TestGL()
{
    if (!graphics_initialized)
        g_vm->BuiltinError("graphics system not initialized yet, call gl_window() first");
}

float2 localpos(const int2 &pos) { return (otransforms.view2object * float4(float3(float2(pos), 0), 1)).xyz().xy(); }
float2 localfingerpos(int i) { return localpos(GetFinger(i, false)); }

Value PushTransform(const float4x4 &forward, const float4x4 &backward, const Value &body)
{
    if (body.True())
    {
        g_vm->Push(Value(g_vm->NewString((char *)&otransforms, sizeof(objecttransforms))));
    }
    AppendTransform(forward, backward);
    return body;
}

void PopTransform()
{
    auto s = g_vm->Pop();
    TYPE_ASSERT(s.type == V_STRING);
    assert(s.sval()->len == sizeof(objecttransforms));
    otransforms = *(objecttransforms *)s.sval()->str();
    s.DECRT();
}

Mesh *GetMesh(Value &i)
{
    auto m = meshes->Get(i.ival());
    if (!m) g_vm->BuiltinError("graphics: illegal mesh id: " + to_string(i.ival()));
    return m;
}

int GetSampler(Value &i)
{
    if (i.ival() < 0 || i.ival() >= Shader::MAX_SAMPLERS)
        g_vm->BuiltinError("graphics: illegal texture unit");
    return i.ival();
}

Mesh *CreatePolygon(Value &vl)
{
    TestGL();

    if (vl.eval()->Len() < 3) g_vm->BuiltinError("polygon: must have at least 3 verts");

    auto vbuf = new BasicVert[vl.eval()->Len()];
    for (int i = 0; i < vl.eval()->Len(); i++) vbuf[i].pos = ValueToF<3>(vl.eval()->At(i));

    auto v1 = vbuf[1].pos - vbuf[0].pos;
    auto v2 = vbuf[2].pos - vbuf[0].pos;
    auto norm = normalize(cross(v2, v1));
    for (int i = 0; i < vl.eval()->Len(); i++)
    {
        vbuf[i].norm = norm;
        vbuf[i].tc = vbuf[i].pos.xy();
        vbuf[i].col = byte4_255;
    }

    auto m = new Mesh(new Geometry(vbuf, vl.eval()->Len(), sizeof(BasicVert), "PNTC"), polymode);

    delete[] vbuf;
    return m;
}

void AddGraphics()
{
    STARTDECL(gl_window) (Value &title, Value &xs, Value &ys, Value &fullscreen, Value &novsync)
    {
        if (graphics_initialized)
            g_vm->BuiltinError("cannot call gl_window() twice");

        string err = SDLInit(title.sval()->str(), int2(xs.ival(), ys.ival()), fullscreen.ival() != 0, novsync.ival() == 0);
        title.DECRT();

        if (err.empty())
        {
            OpenGLInit();

            err = LoadMaterialFile("shaders/default.materials");
        }

        if (!err.empty())
        {
            Output(OUTPUT_INFO, err.c_str());
            return Value(g_vm->NewString(err));
        }

        colorshader = LookupShader("color");
        assert(colorshader);

        meshes = new IntResourceManagerCompact<Mesh>([](Mesh *m) { delete m; });

        Output(OUTPUT_INFO, "graphics fully initialized...");
        graphics_initialized = true;

        return Value();
    }
    ENDDECL5(gl_window, "title,xs,ys,fullscreen,novsync", "SIII?I?", "S?",
        "opens a window for OpenGL rendering. returns error string if any problems, nil otherwise.");

    STARTDECL(gl_loadmaterials) (Value &fn, Value &isinline)
    {
        TestGL();
        auto err = isinline.True() ? ParseMaterialFile(fn.sval()->str()) 
                                   : LoadMaterialFile(fn.sval()->str());
        fn.DECRT();
        return err[0] ? Value(g_vm->NewString(err)) : Value();
    }
    ENDDECL2(gl_loadmaterials, "materialdefs,inline", "SI?", "S?",
        "loads an additional materials file (shader/default.materials is already loaded by default by gl_window())."
        " if inline is true, materialdefs is not a filename, but the actual materials."
        " returns error string if any problems, nil otherwise.");

    STARTDECL(gl_frame) ()
    {
        TestGL();

        #ifdef USE_MAIN_LOOP_CALLBACK
            // Here we have to something hacky: emscripten requires us to not take over the main loop.
            // So we use this exception to suspend the VM right inside the gl_frame() call.
            // FIXME: do this at the start of the frame instead?
            throw string("SUSPEND-VM-MAINLOOP");
        #endif

        auto cb = GraphicsFrameStart();

        return Value(!cb);
    }
    ENDDECL0(gl_frame, "", "", "I",
        "advances rendering by one frame, swaps buffers, and collects new input events."
        " returns true if the closebutton on the window was pressed");

    STARTDECL(gl_shutdown) ()
    {
        GraphicsShutDown();
        return Value();
    }
    ENDDECL0(gl_shutdown, "", "", "",
        "shuts down the OpenGL window. you only need to call this function if you wish to close it before the end"
        " of the program");

    STARTDECL(gl_windowtitle) (Value &s)
    {
        TestGL();
        SDLTitle(s.sval()->str());
        return s;
    }
    ENDDECL1(gl_windowtitle, "title", "S", "S",
        "changes the window title.");

    STARTDECL(gl_visible) ()
    {
        return Value(!SDLIsMinimized());
    }
    ENDDECL0(gl_visible, "", "", "I",
        "checks if the window is currently visible (not minimized, or on mobile devices, in the foreground)."
        " If false, you should not render anything, nor run the frame's code.");

    STARTDECL(gl_cursor) (Value &on)
    {
        TestGL();
        return Value(SDLCursor(on.ival() != 0));
    }
    ENDDECL1(gl_cursor, "on", "I", "I",
        "default the cursor is visible, turn off for implementing FPS like control schemes. return wether it's on.");

    STARTDECL(gl_grab) (Value &on)
    {
        TestGL();
        return Value(SDLGrab(on.ival() != 0));
    }
    ENDDECL1(gl_grab, "on", "I", "I",
        "grabs the mouse when the window is active. return wether it's on.");

    STARTDECL(gl_wentdown) (Value &name)
    {
        auto ks = GetKS(name.sval()->str());
        name.DECRT();
        return Value(ks.wentdown);
    }
    ENDDECL1(gl_wentdown, "name", "S", "I",
        "wether a key/mousebutton/finger went down this frame (pass a string like"
        " mouse1/mouse2/mouse3/escape/space/up/down/a/b/f1 etc. mouse11 and on are additional fingers)");

    STARTDECL(gl_wentup) (Value &name)
    {
        auto ks = GetKS(name.sval()->str());
        name.DECRT();
        return Value(ks.wentup);
    }
    ENDDECL1(gl_wentup, "name", "S", "I",
        "wether a key/mousebutton/finger went up this frame");

    STARTDECL(gl_isdown) (Value &name)
    {
        auto ks = GetKS(name.sval()->str());
        name.DECRT();
        return Value(ks.isdown);
    }
    ENDDECL1(gl_isdown, "name", "S", "I",
        "wether a key/mousebutton/finger is currently down");

    STARTDECL(gl_touchscreen) ()
    {
        #ifdef PLATFORM_TOUCH
            return Value(true);
        #else
            return Value(false);
        #endif
    }
    ENDDECL0(gl_touchscreen, "", "", "I",
        "wether a you\'re getting input from a touch screen (as opposed to mouse & keyboard)");

    STARTDECL(gl_dpi) (Value &screen)
    {
        return Value(SDLScreenDPI(screen.ival()));
    }
    ENDDECL1(gl_dpi, "screen", "I", "I",
        "the DPI of the screen. always returns a value for screen 0, any other screens may return 0"
        " to indicate the screen doesn\'t exist");

    STARTDECL(gl_windowsize) ()
    {
        return ToValueI(GetScreenSize());
    }
    ENDDECL0(gl_windowsize, "", "", "I]:2",
        "a vector representing the size (in pixels) of the window, changes when the user resizes");

    STARTDECL(gl_mousepos) (Value &i)
    {
        return ToValueI(GetFinger(i.ival(), false));
    }
    ENDDECL1(gl_mousepos, "i", "I", "I]:2",
        "the current mouse/finger position in pixels, pass a value other than 0 to read additional fingers"
        " (for touch screens only if the corresponding gl_isdown is true)");

    STARTDECL(gl_mousedelta) (Value &i)
    {
        return ToValueI(GetFinger(i.ival(), true));
    }
    ENDDECL1(gl_mousedelta, "i", "I", "I]:2",
        "number of pixels the mouse/finger has moved since the last frame. use this instead of substracting positions"
        " to correctly deal with lifted fingers and FPS mode (gl_cursor(0))");

    STARTDECL(gl_localmousepos) (Value &i)
    {
        return ToValueF(localfingerpos(i.ival()));
    }
    ENDDECL1(gl_localmousepos, "i", "I", "F]:2",
        "the current mouse/finger position local to the current transform (gl_translate etc)"
        " (for touch screens only if the corresponding gl_isdown is true)");

    STARTDECL(gl_lastpos) (Value &name, Value &on)
    {
        auto p = GetKeyPos(name.sval()->str(), on.ival());
        name.DECRT();
        return ToValueI(p);
    }
    ENDDECL2(gl_lastpos, "name,down", "SI", "I]:2",
        "position (in pixels) key/mousebutton/finger last went down (true) or up (false)");

    STARTDECL(gl_locallastpos) (Value &name, Value &on)
    {
        auto p = localpos(GetKeyPos(name.sval()->str(), on.ival()));
        name.DECRT();
        return ToValueF(p);
    }
    ENDDECL2(gl_locallastpos, "name,down", "SI", "F]:2",
        "position (local to the current transform) key/mousebutton/finger last went down (true) or up (false)");

    STARTDECL(gl_mousewheeldelta) ()
    {
        return Value(SDLWheelDelta());
    }
    ENDDECL0(gl_mousewheeldelta, "", "", "I",
        "amount the mousewheel scrolled this frame, in number of notches");

    STARTDECL(gl_joyaxis) (Value &i)
    {
        return Value(GetJoyAxis(i.ival()));
    }
    ENDDECL1(gl_joyaxis, "i", "I", "F",
        "the current joystick orientation for axis i, as -1 to 1 value");

    STARTDECL(gl_deltatime) ()
    {
        return Value((float)SDLDeltaTime());
    }
    ENDDECL0(gl_deltatime, "", "", "F",
        "seconds since the last frame, updated only once per frame");

    STARTDECL(gl_time) ()
    {
        return Value((float)SDLTime());
    }
    ENDDECL0(gl_time, "", "", "F",
        "seconds since the start of the OpenGL subsystem, updated only once per frame (use seconds_elapsed()"
        " for continuous timing)");

    STARTDECL(gl_lasttime) (Value &name, Value &on)
    {
        auto t = GetKeyTime(name.sval()->str(), on.ival());
        name.DECRT();
        return Value((float)t);
    }
    ENDDECL2(gl_lasttime, "name,down", "SI", "F",
        "time key/mousebutton/finger last went down (true) or up (false)");

    STARTDECL(gl_clear) (Value &col)
    {
        TestGL();
        ClearFrameBuffer(ValueDecToF<3>(col));
        return Value();
    }
    ENDDECL1(gl_clear, "col", "F]", "",
        "clears the framebuffer (and depth buffer) to the given color");

    STARTDECL(gl_color) (Value &col, Value &body)
    {
        if (body.True()) g_vm->Push(ToValueF(curcolor));  // FIXME: maybe more efficient as an int
        curcolor = ValueDecToF<4>(col);
        return body;
    }
    MIDDECL(gl_color) ()
    {
        curcolor = ValueDecToF<4>(g_vm->Pop());
    }
    ENDDECL2CONTEXIT(gl_color, "col,body", "F]C?", "",
        "sets the current color. when a body is given, restores the previous color afterwards");

    STARTDECL(gl_polygon) (Value &vl)
    {
        auto m = CreatePolygon(vl);
        currentshader->Set();
        m->Render(currentshader);
        delete m;
        return vl;
    }
    ENDDECL1(gl_polygon, "vertlist", "F]]", "A1",
        "renders a polygon using the list of points given. returns the argument."
        " warning: gl_polygon creates a new mesh every time, gl_newpoly/gl_rendermesh is faster.");

    STARTDECL(gl_circle) (Value &radius, Value &segments)
    {
        TestGL();

        RenderCircle(currentshader, polymode, max(segments.ival(), 3), radius.fval());

        return Value();
    }
    ENDDECL2(gl_circle, "radius,segments", "FI", "",
        "renders a circle");

    STARTDECL(gl_opencircle) (Value &radius, Value &segments, Value &thickness)
    {
        TestGL();

        RenderOpenCircle(currentshader, max(segments.ival(), 3), radius.fval(), thickness.fval());

        return Value();
    }
    ENDDECL3(gl_opencircle, "radius,segments,thickness", "FIF", "",
        "renders a circle that is open on the inside. thickness is the fraction of the radius that is filled,"
        " try e.g. 0.2");

    STARTDECL(gl_unitcube) ()
    {
        RenderUnitCube(currentshader);
        return Value();
    }
    ENDDECL0(gl_unitcube, "", "", "",
        "renders a unit cube (0,0,0) - (1,1,1)");

    STARTDECL(gl_rotate_x) (Value &angle, Value &body)
    {
        auto a = ValueDecToF<2>(angle);
        return PushTransform(rotationX(a), rotationX(a * float2(1, -1)), body);
    }
    MIDDECL(gl_rotate_x) ()
    {
        PopTransform();
    }
    ENDDECL2CONTEXIT(gl_rotate_x, "vector,body", "F]:2C?", "",
        "rotates the yz plane around the x axis, using a 2D vector normalized vector as angle."
        " when a body is given, restores the previous transform afterwards");

    STARTDECL(gl_rotate_y) (Value &angle, Value &body)
    {
        auto a = ValueDecToF<2>(angle);
        return PushTransform(rotationY(a), rotationY(a * float2(1, -1)), body);
    }
    MIDDECL(gl_rotate_y) ()
    {
        PopTransform();
    }
    ENDDECL2CONTEXIT(gl_rotate_y, "angle,body", "F]:2C?", "",
        "rotates the xz plane around the y axis, using a 2D vector normalized vector as angle."
        " when a body is given, restores the previous transform afterwards");

    STARTDECL(gl_rotate_z) (Value &angle, Value &body)
    {
        auto a = ValueDecToF<2>(angle);
        return PushTransform(rotationZ(a), rotationZ(a * float2(1, -1)), body);
    }
    MIDDECL(gl_rotate_z) ()
    {
        PopTransform();
    }
    ENDDECL2CONTEXIT(gl_rotate_z, "angle,body", "F]:2C?", "",
        "rotates the xy plane around the z axis (used in 2D), using a 2D vector normalized vector as angle."
        " when a body is given, restores the previous transform afterwards");

    STARTDECL(gl_translate) (Value &vec, Value &body)
    {
        auto v = ValueDecToF<3>(vec);
        return PushTransform(translation(v), translation(-v), body);
    }
    MIDDECL(gl_translate) ()
    {
        PopTransform();
    }
    ENDDECL2CONTEXIT(gl_translate, "vec,body", "F]C?", "",
        "translates the current coordinate system along a vector. when a body is given,"
        " restores the previous transform afterwards");

    STARTDECL(gl_scale) (Value &f, Value &body)
    {
        auto v = f.fval() * float3_1;
        return PushTransform(float4x4(float4(v, 1)), float4x4(float4(float3_1 / v, 1)), body);
    }
    MIDDECL(gl_scale) ()
    {
        PopTransform();
    }
    ENDDECL2CONTEXIT(gl_scale, "factor,body", "FC?", "",
        "scales the current coordinate system using a numerical factor."
        " when a body is given, restores the previous transform afterwards");

    STARTDECL(gl_scale) (Value &vec, Value &body)
    {
        auto v = ValueDecToF<3>(vec, 1);
        return PushTransform(float4x4(float4(v, 1)), float4x4(float4(float3_1 / v, 1)), body);
    }
    MIDDECL(gl_scale) ()
    {
        PopTransform();
    }
    ENDDECL2CONTEXIT(gl_scale, "factor,body", "F]C?", "",
        "scales the current coordinate system using a vector."
        " when a body is given, restores the previous transform afterwards");

    STARTDECL(gl_origin) ()
    {
        auto pos = float2(otransforms.object2view[3].x(), otransforms.object2view[3].y());
        return ToValueF(pos);
    }
    ENDDECL0(gl_origin, "", "", "F]:2",
        "returns a vector representing the current transform origin in pixels."
        " only makes sense in 2D mode (no gl_perspective called).");

    STARTDECL(gl_scaling) ()
    {
        auto sc = float2(otransforms.object2view[0].x(), otransforms.object2view[1].y());
        return ToValueF(sc);
    }
    ENDDECL0(gl_scaling, "", "", "F]:2",
        "returns a vector representing the current transform scale in pixels."
        " only makes sense in 2D mode (no gl_perspective called).");

    STARTDECL(gl_pointscale) (Value &f)
    {
        custompointscale = f.fval();
        return Value();
    }
    ENDDECL1(gl_pointscale, "factor", "F", "",
        "sets the current scaling factor for point sprites."
        " this can be what the current gl_scale is, or different, depending on the desired visuals."
        " the ideal size may also be FOV dependent.");

    STARTDECL(gl_linemode) (Value &on, Value &body)
    {
        if (body.True()) g_vm->Push(Value((int)polymode));
        polymode = on.ival() ? PRIM_LOOP : PRIM_FAN;
        return body;
    }
    MIDDECL(gl_linemode) ()
    {
        polymode = (Primitive)g_vm->Pop().ival();
    }
    ENDDECL2CONTEXIT(gl_linemode, "on,body", "IC", "",
        "set line mode (true == on). when a body is given,"
        " restores the previous mode afterwards");

    STARTDECL(gl_hit) (Value &vec, Value &i)
    {
        auto size = ValueDecToF<3>(vec);
        auto localmousepos = localfingerpos(i.ival());
        auto hit = localmousepos.x() >= 0 &&
                   localmousepos.y() >= 0 &&
                   localmousepos.x() < size.x() &&
                   localmousepos.y() < size.y();
        if (hit) lasthitsize = size;
        /*
        #ifdef PLATFORM_TOUCH
        // Inefficient for fingers other than 0, which is going to be rare. 
        auto ks = i ? GetKS((string("mouse1") + (char)('0' + i)).c_str()) : GetKS("mouse1");
        // On mobile, if the finger just went down, we wont have meaningfull lastframehitsize, so if the
        // programmer checks for the combination of gl_hit and gl_wentdown, that would fail.
        // Instead, we bypass that check.
        // PROBLEM: now we'll be returning true for overlapping elements.
        // if we can solve this, we can remove the frame delay from the input system.
        if (ks.wentdown && hit) return true;
        #endif
        */
        return Value(size == lastframehitsize && hit);
    }
    ENDDECL2(gl_hit, "vec,i", "F]I", "I",
        "wether the mouse/finger is inside of the rectangle specified in terms of the current transform"
        " (for touch screens only if the corresponding gl_isdown is true). Only true if the last rectangle for"
        " which gl_hit was true last frame is of the same size as this one (allows you to safely test in most cases"
        " of overlapping rendering)");

    STARTDECL(gl_rect) (Value &vec, Value &centered)
    {
        TestGL();
        RenderQuad(currentshader, polymode, centered.True(), float4x4(float4(ValueToF<2>(vec), 1)));
        return vec;
    }
    ENDDECL2(gl_rect, "vec,centered", "F]I?", "F]",
        "renders a rectangle (0,0)..(1,1) (or (-1,-1)..(1,1) when centered), scaled by the given size."
        " returns the argument.");

    STARTDECL(gl_unit_square) (Value &centered)
    {
        TestGL();
        RenderUnitSquare(currentshader, polymode, centered.True());
        return Value();
    }
    ENDDECL1(gl_unit_square, "centered", "I?", "",
        "renders a square (0,0)..(1,1) (or (-1,-1)..(1,1) when centered)");

    STARTDECL(gl_line) (Value &start, Value &end, Value &thickness)
    {
        TestGL();

        auto v1 = ValueDecToF<3>(start);
        auto v2 = ValueDecToF<3>(end);

        RenderLine2D(currentshader, polymode, v1, v2, thickness.fval());
        return Value();
    }
    ENDDECL3(gl_line, "start,end,thickness", "F]F]F", "",
        "renders a line with the given thickness");

    STARTDECL(gl_perspective) (Value &fovy, Value &znear, Value &zfar)
    {
        Set3DMode(fovy.fval()*RAD, GetScreenSize().x() / (float)GetScreenSize().y(), znear.fval(), zfar.fval());
        return Value();
    }
    ENDDECL3(gl_perspective, "fovy,znear,zfar", "FFF", "",
        "changes from 2D mode (default) to 3D perspective mode with vertical fov (try 60), far plane"
        " (furthest you want to be able to render, try 1000) and near plane (try 1)");

    STARTDECL(gl_ortho) ()
    {
        Set2DMode(GetScreenSize(), true);
        return Value();
    }
    ENDDECL0(gl_ortho, "", "", "",
        "changes back to 2D mode rendering with a coordinate system from (0,0) top-left to the screen size in pixels"
        " bottom right. this is the default at the start of a frame, use this call to get back to that after"
        " gl_perspective.");

    STARTDECL(gl_ortho3d) (Value &center, Value &extends)
    {
        Set3DOrtho(ValueDecToF<3>(center), ValueDecToF<3>(extends));
        return Value();
    }
    ENDDECL2(gl_ortho3d, "center,extends", "F]F]", "",
        "sets a custom ortho projection as 3D projection.");

    STARTDECL(gl_newmesh) (Value &format, Value &attributes, Value &indices)
    {
        TestGL();

        auto nattr = format.sval()->len;
        if (nattr < 1 || nattr > 10 || nattr != attributes.vval()->len)
            g_vm->BuiltinError("newmesh: illegal format/attributes size");

        auto fmt = format.sval()->str();
        if (nattr != (int)strspn(fmt, "PCTN") || fmt[0] != 'P')
            g_vm->BuiltinError("newmesh: illegal format characters (only PCTN allowed), P must be first");

        auto attrs = attributes.vval();
        auto positions = attrs->At(0);

        vector<int> idxs;
        if (indices.True())
        {
            for (int i = 0; i < indices.eval()->Len(); i++)
            {
                auto &e = indices.eval()->At(i);
                if (e.ival() < 0 || e.ival() >= positions.eval()->Len())
                    g_vm->BuiltinError("newmesh: index out of range of vertex list");
                idxs.push_back(e.ival());
            }
            indices.DECRT();
        }

        int nverts = positions.eval()->Len();
        size_t vsize = AttribsSize(fmt);
        size_t normal_offset = 0;
        auto verts = new uchar[nverts * vsize];

        for (int i = 0; i < nverts; i++)
        {
            auto start = &verts[i * vsize];
            auto p = start;
            auto fmt_it = fmt;
            float3 pos;
            while (*fmt_it)
            {
                auto attrv = attrs->At(fmt_it - fmt).vval();
                switch (*fmt_it++)
                {
                    case 'P':
                        *((float3 *&)p)++ = pos = ValueToF<3>(attrv->At(i));
                        break;
                    case 'C':
                        *((byte4  *&)p)++ = i < attrv->Len() ? quantizec(ValueToF<4>(attrv->At(i), 1)) : byte4_255;
                        break;
                    case 'T':
                        *((float2 *&)p)++ = i < attrv->Len() ? ValueToF<2>(attrv->At(i), 0) : pos.xy();
                        break;
                    case 'N':
                        if (!attrv->Len()) normal_offset = p - start;
                        *((float3 *&)p)++ = i < attrv->Len() ? ValueToF<3>(attrv->At(i), 0) : float3_0;
                        break;
                    default: assert(0);
                }
            } 
        }

        if (normal_offset)
        {
            // if no normals were specified, generate them.
            normalize_mesh(&idxs[0], idxs.size(), verts, nverts, vsize, normal_offset);
        }

        format.DECRT();
        attributes.DECRT();

        // FIXME: make meshes into points in a more general way.
        auto m = new Mesh(new Geometry(verts, nverts, vsize, fmt), indices.True() ? PRIM_TRIS : PRIM_POINT);
        if (idxs.size()) m->surfs.push_back(new Surface(&idxs[0], idxs.size()));

        delete[] verts;

        return Value((int)meshes->Add(m));
    }
    ENDDECL3(gl_newmesh, "format,attributes,indices", "SF]]]I]?", "I",
        "creates a new vertex buffer and returns an integer id (1..) for it."
        " format must be made up of characters P (position), C (color), T (texcoord), N (normal)."
        " position is obligatory and must come first."
        " attributes is a vector with the same number of attribute vectors as format elements."
        " you may specify [] to get defaults for colors (white) / texcoords (position x & y) /"
        " normals (generated from adjacent triangles)."
        " example: mymesh := gl_newmesh(\"PCN\", [ positions, colors, [] ], indices)");

    STARTDECL(gl_newpoly) (Value &positions)
    {
        auto m = CreatePolygon(positions);
        positions.DECRT();
        return Value((int)meshes->Add(m));
    }
    ENDDECL1(gl_newpoly, "positions", "F]]", "I",
        "creates a mesh out of a loop of points, much like gl_polygon."
        " gl_linemode determines how this gets drawn (fan or loop)."
        " returns mesh id");

    STARTDECL(gl_newmesh_iqm) (Value &fn)
    {
        TestGL();

        auto m = LoadIQM(fn.sval()->str());
        fn.DECRT();
        return Value(m ? (int)meshes->Add(m) : 0);
    }
    ENDDECL1(gl_newmesh_iqm, "filename", "S", "I",
        "load a .iqm file into a mesh, returns integer id (1..), or 0 on failure to load.");

    STARTDECL(gl_deletemesh) (Value &i)
    {
        meshes->Delete(i.ival());
        return Value();
    }
    ENDDECL1(gl_deletemesh, "i", "I", "",
        "free up memory for the given mesh id");

    STARTDECL(gl_meshparts) (Value &i)
    {
        auto m = GetMesh(i);
        auto v = (LVector *)g_vm->NewVector(0, (int)m->surfs.size(), g_vm->GetTypeInfo(TYPE_ELEM_VECTOR_OF_STRING));
        for (auto s : m->surfs) v->Push(Value(g_vm->NewString(s->name)));
        return Value(v);
    }
    ENDDECL1(gl_meshparts, "i", "I", "S]",
        "returns an array of names of all parts of mesh i (names may be empty)");

    STARTDECL(gl_meshsize) (Value &i)
    {
        auto m = GetMesh(i);
        return Value((int)m->geom->nverts);
    }
    ENDDECL1(gl_meshsize, "i", "I", "I",
        "returns the number of verts in this mesh");

    STARTDECL(gl_animatemesh) (Value &i, Value &f)
    {
        GetMesh(i)->curanim = f.fval();
        return Value();
    }
    ENDDECL2(gl_animatemesh, "i,frame", "IF", "",
        "set the frame for animated mesh i");

    STARTDECL(gl_rendermesh) (Value &i)
    {
        TestGL();
        GetMesh(i)->Render(currentshader);
        return Value();
    }
    ENDDECL1(gl_rendermesh, "i", "I", "",
        "renders the specified mesh");

    STARTDECL(gl_setshader) (Value &shader)
    {
        TestGL();

        auto sh = LookupShader(shader.sval()->str());

        if (!sh) g_vm->BuiltinError(string("no such shader: ") + shader.sval()->str());

        shader.DECRT();

        currentshader = sh;

        return Value();
    }
    ENDDECL1(gl_setshader, "shader", "S", "",
        "changes the current shader. shaders must reside in the shaders folder, builtin ones are:"
        " color / textured / phong");

    STARTDECL(gl_setuniform) (Value &name, Value &vec)
    {
        TestGL();
        auto len = vec.eval()->Len();
        auto v = ValueDecToF<4>(vec);
        currentshader->Activate();
        auto ok = currentshader->SetUniform(name.sval()->str(), v.begin(), len);
        name.DECRT();
        return Value(ok);
    }
    ENDDECL2(gl_setuniform, "name,value", "SF]", "I",
             "set a uniform on the current shader. size of float vector must match size of uniform in the shader."
             " returns false on error.");

    STARTDECL(gl_setuniformarray) (Value &name, Value &vec)
    {
        TestGL();
        vector<float4> vals(vec.eval()->Len());
        for (int i = 0; i < vec.eval()->Len(); i++) vals[i] = ValueToF<4>(vec.eval()->At(i));
        vec.DECRT();
        currentshader->Activate();
        auto ok = currentshader->SetUniform(name.sval()->str(), vals.data()->data(), 4, (int)vals.size());
        name.DECRT();
        return Value(ok);
    }
    ENDDECL2(gl_setuniformarray, "name,value", "SF]]", "I",
             "set a uniform on the current shader. uniform in the shader must be an array of vec4."
             " returns false on error.");

    STARTDECL(gl_uniformbufferobject) (Value &name, Value &vec, Value &ssbo)
    {
        TestGL();
        vector<float4> vals(vec.eval()->Len());
        for (int i = 0; i < vec.eval()->Len(); i++) vals[i] = ValueToF<4>(vec.eval()->At(i));
        vec.DECRT();
        auto id = UniformBufferObject(currentshader, vals.data()->data(), 4 * vals.size(), name.sval()->str(), ssbo.True());
        name.DECRT();
        return Value((int)id);
    }
    ENDDECL3(gl_uniformbufferobject, "name,value,ssbo", "SF]]I?", "I",
             "creates a uniform buffer object, and attaches it to the current shader at the given uniform block name."
             " uniforms in the shader must be all vec4s, or an array of them."
             " ssbo indicates if you want a shader storage block instead."
             " returns buffer id or 0 on error.");

    STARTDECL(gl_deletebufferobject) (Value &id)
    {
        TestGL();

        // FIXME: should route this thru a IntResourceManagerCompact to be safe?
        // I guess GL doesn't care about illegal id's?
        DeleteBO(id.ival());

        return Value();
    }
    ENDDECL1(gl_deletebufferobject, "id", "I", "",
        "deletes a buffer objects, e.g. one allocated by gl_uniformbufferobject().");


    STARTDECL(gl_bindmeshtocompute) (Value &mesh, Value &bpi)
    {
        TestGL();
        if (mesh.ival()) GetMesh(mesh)->geom->BindAsSSBO(bpi.ival());
        else BindVBOAsSSBO(bpi.ival(), 0);
        return Value();
    }
    ENDDECL2(gl_bindmeshtocompute, "mesh,binding", "II", "",
        "Bind the vertex data of a mesh to a SSBO binding of a compute shader. Pass a 0 mesh to unbind.");

    STARTDECL(gl_dispatchcompute) (Value &groups)
    {
        TestGL();

        DispatchCompute(ValueDecToI<3>(groups));

        return Value();
    }
    ENDDECL1(gl_dispatchcompute, "groups", "I]", "",
             "dispatches the currently set compute shader in groups of sizes of the specified x/y/z values.");

    STARTDECL(gl_blend) (Value &mode, Value &body)
    {
        TestGL();

        int old = SetBlendMode((BlendMode)mode.ival());
        if (body.True()) g_vm->Push(Value(old));
        return body;
    }
    MIDDECL(gl_blend) ()
    {
        auto m = g_vm->Pop();
        TYPE_ASSERT(m.type == V_INT);
        SetBlendMode((BlendMode)m.ival());
    }
    ENDDECL2CONTEXIT(gl_blend, "on,body", "IC?", "",
        "changes the blending mode to 0: off, 1: alpha blend (default), 2: additive, 3: alpha additive,"
        " 4: multiplicative. when a body is given, restores the previous mode afterwards");

    STARTDECL(gl_loadtexture) (Value &name, Value &tf)
    {
        TestGL();

        uint id = 0;
        int2 dim(0);
        auto it = texturecache.find(name.sval()->str());
        if (it != texturecache.end())
        {
            id = it->second;
        }
        else
        {
            id = CreateTextureFromFile(name.sval()->str(), dim, tf.ival());

            if (id) texturecache[name.sval()->str()] = id;
        }
        name.DECRT();
        g_vm->Push(Value((int)id));
        return ToValueI(dim);
    }
    ENDDECL2(gl_loadtexture, "name,textureformat", "SI?", "II]:2",
        "returns texture id if succesfully loaded from file name, otherwise 0."
        " see color.lobster for texture format."
        " Returns the size of the loaded textures in pixels as second return value on first load (xy_i),"
        " or (0, 0) otherwise."
        " Only loads from disk once if called again with the same name. Uses stb_image internally"
        " (see http://nothings.org/), loads JPEG Baseline, subsets of PNG, TGA, BMP, PSD, GIF, HDR, PIC.");

    STARTDECL(gl_setprimitivetexture) (Value &i, Value &id, Value &tf)
    {
        TestGL();

        SetTexture(GetSampler(i), id.ival(), tf.ival());

        return Value();
    }
    ENDDECL3(gl_setprimitivetexture, "i,id,textureformat", "III?", "",
        "sets texture unit i to texture id (for use with rect/circle/polygon/line)");

    STARTDECL(gl_setmeshtexture) (Value &mid, Value &part, Value &i, Value &id)
    {
        auto m = GetMesh(mid);

        if (part.ival() < 0 || part.ival() >= (int)m->surfs.size())
            g_vm->BuiltinError("setmeshtexture: illegal part index");

        m->surfs[part.ival()]->textures[GetSampler(i)] = id.ival();

        return Value();
    }
    ENDDECL4(gl_setmeshtexture, "meshid,part,i,textureid", "IIII", "",
        "sets texture unit i to texture id for a mesh and part (0 if not a multi-part mesh)");

    STARTDECL(gl_setimagetexture) (Value &i, Value &id, Value &tf)
    {
        TestGL();

        SetImageTexture(GetSampler(i), id.ival(), tf.ival());

        return Value();
    }
    ENDDECL3(gl_setimagetexture, "i,id,textureformat", "III", "",
             "sets image unit i to texture id (for use with compute)."
             " texture format must be the sames as what you specified in gl_loadtexture/gl_createtexture,"
             " with optionally writeonly/readwrite flags.");

    STARTDECL(gl_createtexture) (Value &matv, Value &tf)
    {
        TestGL();

        ElemObj *mat = matv.eval();
        int ys = mat->Len();
        int xs = mat->At(0).eval()->Len();
        auto sz = tf.ival() & TF_FLOAT ? sizeof(float4) : sizeof(byte4);
        auto buf = new uchar[xs * ys * sz];
        memset(buf, 0, xs * ys * sz);
        for (int i = 0; i < ys; i++)
        {
            ElemObj *row = mat->At(i).eval();
            for (int j = 0; j < min(xs, row->Len()); j++)
            {
                float4 col = ValueToF<4>(row->At(j));
                auto idx = i * xs + j;
                if (tf.ival() & TF_FLOAT) ((float4 *)buf)[idx] = col;
                else                      ((byte4  *)buf)[idx] = quantizec(col);
            }
        }
        matv.DECRT();
        uint id = CreateTexture(buf, int2(xs, ys), tf.ival());
        delete[] buf;
        return Value((int)id);
    }
    ENDDECL2(gl_createtexture, "matrix,textureformat", "F]]]I?", "I",
        "creates a texture from a 2d array of color vectors, returns texture id."
        " see color.lobster for texture format");

    STARTDECL(gl_createblanktexture) (Value &size_, Value &col, Value &tf)
    {
        TestGL();

        return Value((int)CreateBlankTexture(ValueDecToI<2>(size_), ValueDecToF<4>(col), tf.ival()));
    }
    ENDDECL3(gl_createblanktexture, "size,color,textureformat", "I]F]I?", "I",
             "creates a blank texture (for use as frame buffer or with compute shaders), returns texture id."
             " see color.lobster for texture format");

    STARTDECL(gl_deletetexture) (Value &i)
    {
        TestGL();
        uint tex = i.ival();

        auto it = texturecache.begin();
        // this is potentially expensive, we're counting on gl_deletetexture not being needed often
        while (it != texturecache.end())
        {
            if (it->second == uint(tex)) texturecache.erase(it++);
            else ++it;
        }

        // the surfaces in meshes are still potentially referring to this texture,
        // but OpenGL doesn't care about illegal texture ids, so neither do we
        DeleteTexture(tex);

        return Value();
    }
    ENDDECL1(gl_deletetexture, "i", "I", "",
        "free up memory for the given texture id");
    
    STARTDECL(gl_switchtoframebuffer) (Value &tex, Value &fbsize, Value &depth, Value &tf, Value &retex)
    {
        TestGL();
        
        auto sz = fbsize.True() ? ValueDecToI<2>(fbsize) : int2_0;
        return Value(SwitchToFrameBuffer(tex.ival(), tex.ival() ? sz : GetScreenSize(), depth.True(), tf.ival(), retex.ival()));
    }
    ENDDECL5(gl_switchtoframebuffer, "texid,fbsize,hasdepth,textureformat,resolvetex", "II]?I?I?I?", "I",
             "switches to a new framebuffer, that renders into the given texture. pass the texture size."
             " also allocates a depth buffer for it if depth is true."
             " pass the textureformat that was used for this texture."
             " pass a resolve texture if the base texture is multisample."
             " pass a texid of 0 to switch back to the original framebuffer");

    STARTDECL(gl_light) (Value &pos, Value &params)
    {
        Light l;
        l.pos = otransforms.object2view * float4(ValueDecToF<3>(pos), 1);
        l.params = ValueDecToF<2>(params);
        lights.push_back(l);
        return Value();
    }
    ENDDECL2(gl_light, "pos,params", "F]F]", "",
        "sets up a light at the given position for this frame. make sure to call this after your camera transforms"
        " but before any object transforms (i.e. defined in \"worldspace\")."
        " params contains specular exponent in x (try 32/64/128 for different material looks) and"
        " the specular scale in y (try 1 for full intensity)");

    STARTDECL(gl_debug_grid) (Value &num, Value &dist, Value &thickness)
    {
        TestGL();

        float3 cp = otransforms.view2object[3].xyz();
        auto m = float3(ValueDecToI<3>(num));
        auto step = ValueDecToF<3>(dist);

        auto oldcolor = curcolor;
        curcolor = float4(0, 1, 0, 1); for (float z = 0; z <= m.z(); z += step.x()) for (float x = 0; x <= m.x(); x += step.x()) { RenderLine3D(currentshader, float3(x, 0, z), float3(x, m.y(), z), cp, thickness.fval()); }
        curcolor = float4(1, 0, 0, 1); for (float z = 0; z <= m.z(); z += step.y()) for (float y = 0; y <= m.y(); y += step.y()) { RenderLine3D(currentshader, float3(0, y, z), float3(m.x(), y, z), cp, thickness.fval()); }
        curcolor = float4(0, 0, 1, 1); for (float y = 0; y <= m.y(); y += step.z()) for (float x = 0; x <= m.x(); x += step.z()) { RenderLine3D(currentshader, float3(x, y, 0), float3(x, y, m.z()), cp, thickness.fval()); }
        curcolor = oldcolor;

        return Value();
    }
    ENDDECL3(gl_debug_grid, "num,dist,thickness", "I]F]F", "",
        "renders a grid in space for debugging purposes. num is the number of lines in all 3 directions,"
        " and dist their spacing. thickness of the lines in the same units");

    STARTDECL(gl_screenshot) (Value &fn)
    {
        bool ok = ScreenShot(fn.sval()->str());
        fn.DECRT();
        return Value(ok);
    }
    ENDDECL1(gl_screenshot, "filename", "S", "I",
             "saves a screenshot, returns true if succesful");

}
