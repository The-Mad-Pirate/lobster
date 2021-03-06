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
#include "glincludes.h"

#include "sdlinterface.h"
#include "sdlincludes.h"

#ifdef PLATFORM_VR

#include "openvr.h"

vr::IVRSystem *vrsys = nullptr;
vr::IVRRenderModels *vrmodels = nullptr;
vr::TrackedDevicePose_t trackeddeviceposes[vr::k_unMaxTrackedDeviceCount];

string GetTrackedDeviceString(vr::TrackedDeviceIndex_t device, vr::TrackedDeviceProperty prop)
{
    assert(vrsys);
    uint32_t buflen = vrsys->GetStringTrackedDeviceProperty(device, prop, nullptr, 0, nullptr);
    if(buflen == 0) return "";

    char *buf = new char[buflen];
    buflen = vrsys->GetStringTrackedDeviceProperty(device, prop, buf, buflen, nullptr);
    std::string s = buf;
    delete [] buf;
    return s;
}

#endif  // PLATFORM_VR

int2 rtsize = int2_0;
uint mstex[2] = { 0, 0 };
uint retex[2] = { 0, 0 };
struct MotionController { float4x4 mat; uint device; vr::VRControllerState_t state, laststate; };
vector<MotionController> motioncontrollers;
map<string, int> motioncontrollermeshes;

map<string, vr::EVRButtonId> button_ids;

void VRShutDown()
{
    #ifdef PLATFORM_VR
    
    button_ids.clear();

    vrsys = NULL;
    vr::VR_Shutdown();

    for (int i = 0; i < 2; i++)
    {
        DeleteTexture(mstex[i]);
        DeleteTexture(retex[i]);
    }

    #endif  // PLATFORM_VR
}

bool VRInit()
{
    #ifdef PLATFORM_VR

    if (vrsys) return true;

    button_ids["system"]   = vr::k_EButton_System;
    button_ids["menu"]     = vr::k_EButton_ApplicationMenu;
    button_ids["grip"]     = vr::k_EButton_Grip;
    button_ids["touchpad"] = vr::k_EButton_SteamVR_Touchpad;
    button_ids["trigger"]  = vr::k_EButton_SteamVR_Trigger;

    if (!vr::VR_IsHmdPresent()) return false;

    vr::EVRInitError err = vr::VRInitError_None;
    vrsys = vr::VR_Init(&err, vr::VRApplication_Scene);

    if (err != vr::VRInitError_None)
    {
        vrsys = nullptr;
        Output(OUTPUT_ERROR, "VR system init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return false;
    }

    vrsys->GetRecommendedRenderTargetSize((uint *)&rtsize.x_mut(), (uint *)&rtsize.y_mut());

    auto devicename = GetTrackedDeviceString(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String);
    auto displayname = GetTrackedDeviceString(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SerialNumber_String);
    Output(OUTPUT_INFO, "VR running on device: \"%s\", display: \"%s\", rt size: (%d, %d)",
        devicename.c_str(), displayname.c_str(), rtsize.x(), rtsize.y());

    vrmodels = (vr::IVRRenderModels *)vr::VR_GetGenericInterface(vr::IVRRenderModels_Version, &err);
    if(!vrmodels)
    {
        VRShutDown();
        Output(OUTPUT_ERROR, "VR get render models failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return false;
    }

    if (!vr::VRCompositor())
    {
        VRShutDown();
        Output(OUTPUT_ERROR, "VR compositor failed to initialize");
        return false;
    }

    // Get focus?
    vr::VRCompositor()->WaitGetPoses(trackeddeviceposes, vr::k_unMaxTrackedDeviceCount, NULL, 0);

    return true;

    #else

    return false;

    #endif  // PLATFORM_VR
}

float4x4 FromOpenVR(const vr::HmdMatrix44_t &mat) { return float4x4(&mat.m[0][0]).transpose(); }

float4x4 FromOpenVR(const vr::HmdMatrix34_t &mat)
{
    return float4x4(float4(&mat.m[0][0]),
                    float4(&mat.m[1][0]),
                    float4(&mat.m[2][0]),
                    float4(0, 0, 0, 1)).transpose();  // FIXME: simplify
}

void VREye(int eye, float znear, float zfar)
{
    #ifdef PLATFORM_VR

    if (!vrsys) return;

    glEnable(GL_MULTISAMPLE);

    auto retf = TF_CLAMP | TF_NOMIPMAP;
    auto mstf = retf | TF_MULTISAMPLE;
    if (!mstex[eye]) mstex[eye] = CreateBlankTexture(rtsize, float4_0, mstf);
    if (!retex[eye]) retex[eye] = CreateBlankTexture(rtsize, float4_0, retf);

    SwitchToFrameBuffer(mstex[eye], rtsize, true, mstf, retex[eye]);

    auto proj = FromOpenVR(vrsys->GetProjectionMatrix((vr::EVREye)eye, znear, zfar, vr::API_OpenGL));
    Set3DMode(80, 1, znear, zfar);
    view2clip = proj;  // Override the projection set by Set3DMode

    auto eye2head = FromOpenVR(vrsys->GetEyeToHeadTransform((vr::EVREye)eye));
    auto vrview = eye2head;
    if (trackeddeviceposes[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
    {
        auto hmdpose = FromOpenVR(trackeddeviceposes[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking);
        vrview = hmdpose * vrview;
    }
    AppendTransform(invert(vrview), vrview);

    #endif  // PLATFORM_VR
}

void VRFinish()
{
    #ifdef PLATFORM_VR

    if (!vrsys) return;

    SwitchToFrameBuffer(0, GetScreenSize(), false, 0, 0);

    glDisable(GL_MULTISAMPLE);

    for (int i = 0; i < 2; i++)
    {
        vr::Texture_t vrtex = { (void *)retex[i], vr::API_OpenGL, vr::ColorSpace_Gamma };
        auto err = vr::VRCompositor()->Submit((vr::EVREye)i, &vrtex);
        (void)err;
        assert(!err);
    }
    
    vr::VRCompositor()->PostPresentHandoff();

    glFlush();

    auto err = vr::VRCompositor()->WaitGetPoses(trackeddeviceposes, vr::k_unMaxTrackedDeviceCount, NULL, 0);
    (void)err;
    assert(!err);

    size_t mcn = 0;
    for (uint device = 0; device < vr::k_unMaxTrackedDeviceCount; device++)
    {
        if (!trackeddeviceposes[device].bPoseIsValid)
            continue;

        if (vrsys->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller)
            continue;

        if (mcn == motioncontrollers.size())
        {
            MotionController mc;
            memset(&mc, 0, sizeof(mc));
            motioncontrollers.push_back(mc);
        }
        auto &mc = motioncontrollers[mcn];
        mc.mat = FromOpenVR(trackeddeviceposes[device].mDeviceToAbsoluteTracking);
        mc.device = device;
        mc.laststate = mc.state;
        auto ok = vrsys->GetControllerState(device, &mc.state);
        if (!ok) memset(&mc.state, 0, sizeof(vr::VRControllerState_t));
        mcn++;
    }

    #endif  // PLATFORM_VR
}

int VRGetMesh(uint device)
{
    #ifdef PLATFORM_VR

    if (!vrsys) return 0;

    auto name = GetTrackedDeviceString(device, vr::Prop_RenderModelName_String);
    auto it = motioncontrollermeshes.find(name);
    if (it != motioncontrollermeshes.end())
        return it->second;

    vr::RenderModel_t *model = nullptr;
    for (;;)
    {
        auto err = vr::VRRenderModels()->LoadRenderModel_Async(name.c_str(), &model);
        if (err == vr::VRRenderModelError_None)
        {
            break;
        }
        else if (err != vr::VRRenderModelError_Loading)
        {
            return 0;
        }
        SDL_Delay(1);
    }

    vr::RenderModel_TextureMap_t *modeltex = nullptr;
    for (;;)
    {
        auto err = vr::VRRenderModels()->LoadTexture_Async(model->diffuseTextureId, &modeltex);
        if (err == vr::VRRenderModelError_None)
        {
            break;
        }
        else if (err != vr::VRRenderModelError_Loading)
        {
            vr::VRRenderModels()->FreeRenderModel(model);
            return 0;
        }
        SDL_Delay(1);
    }

    auto tex = CreateTexture(modeltex->rubTextureMapData, int2(modeltex->unWidth, modeltex->unHeight), TF_CLAMP);

    auto m = new Mesh(new Geometry(&model->rVertexData[0].vPosition.v[0], model->unVertexCount,
                                   sizeof(vr::RenderModel_Vertex_t), "PNT"), PRIM_TRIS);

    auto nindices = model->unTriangleCount * 3;
    vector<int> indices(nindices);
    for (uint i = 0; i < nindices; i += 3)
    {
        indices[i + 0] = model->rIndexData[i + 0];
        indices[i + 1] = model->rIndexData[i + 2];
        indices[i + 2] = model->rIndexData[i + 1];
    }
    auto surf = new Surface(indices.data(), nindices, PRIM_TRIS);
    surf->textures[0] = tex;
    m->surfs.push_back(surf);

    extern IntResourceManagerCompact<Mesh> *meshes;
    auto midx = (int)meshes->Add(m);

    motioncontrollermeshes[name] = midx;

    vr::VRRenderModels()->FreeRenderModel(model);
    vr::VRRenderModels()->FreeTexture(modeltex);

    return midx;

    #else

    return 0;

    #endif  // PLATFORM_VR
}

using namespace lobster;

MotionController *GetMC(Value &mc)
{
    auto n = mc.ival();
    return n >= 0 && n < (int)motioncontrollers.size()
        ? &motioncontrollers[n]
        : nullptr;
};

vr::EVRButtonId GetButtonId(Value &button)
{
    auto it = button_ids.find(button.sval()->str());
    if (it == button_ids.end()) g_vm->BuiltinError(string("unknown button name: ") + button.sval()->str());
    button.DECRT();
    return it->second;
}

void AddVR()
{
    STARTDECL(vr_init) ()
    {
        return Value(VRInit());
    }
    ENDDECL0(vr_init, "", "", "I",
        "initializes VR mode. returns true if a hmd was found and initialized");

    STARTDECL(vr_starteye) (Value &isright, Value &znear, Value &zfar)
    {
        VREye(isright.True(), znear.fval(), zfar.fval());
        return Value();
    }
    ENDDECL3(vr_starteye, "isright,znear,zfar", "IFF", "",
        "starts rendering for an eye. call for each eye, followed by drawing the world as normal."
        " replaces gl_perspective");

    STARTDECL(vr_finish) ()
    {
        VRFinish();
        return Value();
    }
    ENDDECL0(vr_finish, "", "", "",
        "finishes vr rendering by compositing (and distorting) both eye renders to the screen");

    STARTDECL(vr_geteyetex) (Value &isright)
    {
        return Value((int)retex[isright.True()]);
    }
    ENDDECL1(vr_geteyetex, "isright", "I", "I",
        "returns the texture for an eye. call after vr_finish. can be used to render the non-VR display");

    STARTDECL(vr_nummotioncontrollers) ()
    {
        return Value((int)motioncontrollers.size());
    }
    ENDDECL0(vr_nummotioncontrollers, "", "", "I",
        "returns the number of motion controllers that are currently tracking");

    extern Value PushTransform(const float4x4 &forward, const float4x4 &backward, const Value &body);
    extern void PopTransform();
    STARTDECL(vr_motioncontroller) (Value &mc, Value &body)
    {
        auto mcd = GetMC(mc);
        return mcd
            ? PushTransform(mcd->mat, invert(mcd->mat), body)
            : PushTransform(float4x4_1, float4x4_1, body);
    }
    MIDDECL(vr_motioncontroller) ()
    {
        PopTransform();
    }
    ENDDECL2CONTEXIT(vr_motioncontroller, "n,body", "IC?", "",
        "sets up the transform ready to render controller n."
        " when a body is given, restores the previous transform afterwards."
        " if there is no controller n (or it is currently not"
        " tracking) the identity transform is used");

    STARTDECL(vr_motioncontrollermesh) (Value &mc)
    {
        auto mcd = GetMC(mc);
        return Value(mcd ? VRGetMesh(mcd->device) : 0);
    }
    ENDDECL1(vr_motioncontrollermesh, "n", "I", "I",
        "returns the mesh for motion controller n, or 0 if not available");

    STARTDECL(vr_motioncontrollerbutton) (Value &mc, Value &button)
    {
        auto mcd = GetMC(mc);
        auto mask = ButtonMaskFromId(GetButtonId(button));
        if (!mcd) return Value(0);
        auto masknow = mcd->state.ulButtonPressed & mask;
        auto maskbef = mcd->laststate.ulButtonPressed & mask;
        auto step = int(masknow != 0) * 2 - int(maskbef == 0);
        return Value(step);
    }
    ENDDECL2(vr_motioncontrollerbutton, "n,button", "IS", "I",
        "returns the button state for motion controller n."
        " isdown: >= 1, wentdown: == 1, wentup: == 0, isup: <= 0."
        " buttons are: system, menu, grip, trigger, touchpad");

    STARTDECL(vr_motioncontrollervec) (Value &mc, Value &idx)
    {
        auto mcd = GetMC(mc);
        if (!mcd) return Value(ToValueF(float3_0));
        auto i = RangeCheck(idx, 4);
        return Value(ToValueF(mcd->mat[i].xyz()));
    }
    ENDDECL2(vr_motioncontrollervec, "n,i", "II", "F]:3",
        "returns one of the vectors for motion controller n. 0 = left, 1 = up, 2 = fwd, 4 = pos."
        " These are in Y up space.");
}

