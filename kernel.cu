#pragma once

#include "shared.h"

#define RT_FUNCTION __forceinline__ __device__
#define RT_PROGRAM extern "C" __global__

RT_FUNCTION float3 operator+(const float3 &v0, const float3 &v1) {
    return make_float3(v0.x + v1.x, v0.y + v1.y, v0.z + v1.z);
}
RT_FUNCTION float3 operator*(float s, const float3 &v) {
    return make_float3(s * v.x, s * v.y, s * v.z);
}
RT_FUNCTION float3 operator*(const float3 &v, float s) {
    return make_float3(s * v.x, s * v.y, s * v.z);
}
RT_FUNCTION float3 operator/(const float3 &v, float s) {
    float r = 1 / s;
    return r * v;
}

RT_FUNCTION float length(const float3 &v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}
RT_FUNCTION float3 normalize(const float3 &v) {
    return v / length(v);
}

namespace Sample {

using namespace Shared;

RT_FUNCTION float4 make_float4(const float3 &v, float w) {
    return ::make_float4(v.x, v.y, v.z, w);
}



extern "C" __constant__ PipelineLaunchParameters plp;



// JP: このクラスのようにシステマティックにuint32_t&にせずに、
//     個別に適切なペイロードの渡し方を考えたほうが性能は良いかもしれない。
template <typename PayloadType>
union PayloadAccessor {
    PayloadType raw;
    uint32_t asUInt[(sizeof(PayloadType) + 3) / 4];

    RT_FUNCTION PayloadAccessor() {
        for (int i = 0; i < sizeof(asUInt) / 4; ++i)
            asUInt[i] = optixUndefinedValue();
    }

    RT_FUNCTION uint32_t &operator[](uint32_t idx) {
        return asUInt[idx];
    }

    RT_FUNCTION void getAll() {
        constexpr uint32_t numSlots = sizeof(asUInt) / 4;
        if (numSlots > 0)
            asUInt[0] = optixGetPayload_0();
        if (numSlots > 1)
            asUInt[1] = optixGetPayload_1();
        if (numSlots > 2)
            asUInt[2] = optixGetPayload_2();
        if (numSlots > 3)
            asUInt[3] = optixGetPayload_3();
        if (numSlots > 4)
            asUInt[4] = optixGetPayload_4();
        if (numSlots > 5)
            asUInt[5] = optixGetPayload_5();
        if (numSlots > 6)
            asUInt[6] = optixGetPayload_6();
        if (numSlots > 7)
            asUInt[7] = optixGetPayload_7();
    }

    RT_FUNCTION void setAll() const {
        constexpr uint32_t numSlots = sizeof(asUInt) / 4;
        if (numSlots > 0)
            optixSetPayload_0(asUInt[0]);
        if (numSlots > 1)
            optixSetPayload_1(asUInt[1]);
        if (numSlots > 2)
            optixSetPayload_2(asUInt[2]);
        if (numSlots > 3)
            optixSetPayload_3(asUInt[3]);
        if (numSlots > 4)
            optixSetPayload_4(asUInt[4]);
        if (numSlots > 5)
            optixSetPayload_5(asUInt[5]);
        if (numSlots > 6)
            optixSetPayload_6(asUInt[6]);
        if (numSlots > 7)
            optixSetPayload_7(asUInt[7]);
    }
};



struct Ray {
    float3 origin;
    float3 direction;
    float tmin;
    float tmax;
    float time;

    RT_FUNCTION static Ray getWorld() {
        Ray ret;
        ret.origin = optixGetWorldRayOrigin();
        ret.direction = optixGetWorldRayDirection();
        ret.tmin = optixGetRayTmin();
        ret.tmax = optixGetRayTmax();
        ret.time = optixGetRayTime();
        return ret;
    }
    RT_FUNCTION static Ray getObject() {
        Ray ret;
        ret.origin = optixGetObjectRayOrigin();
        ret.direction = optixGetObjectRayDirection();
        ret.tmin = optixGetRayTmin();
        ret.tmax = optixGetRayTmax();
        ret.time = optixGetRayTime();
        return ret;
    }
};

struct SearchRayPayload {
    float3 contribution;
};

struct VisibilityRayPayload {
    float visibility;
};

struct HitPointParameter {
    float b0, b1;
    int32_t primIndex;

    RT_FUNCTION static HitPointParameter get() {
        HitPointParameter ret;
        if (optixIsTriangleHit()) {
            float2 bc = optixGetTriangleBarycentrics();
            ret.b0 = 1 - bc.x - bc.y;
            ret.b1 = bc.x;
        }
        else {
            ret.b0 = __uint_as_float(optixGetAttribute_0());
            ret.b1 = __uint_as_float(optixGetAttribute_1());
        }
        ret.primIndex = optixGetPrimitiveIndex();
        return ret;
    }
};



RT_PROGRAM void __raygen__fill() {
    uint3 launchIndex = optixGetLaunchIndex();
    int32_t index = plp.imageSize.x * launchIndex.y + launchIndex.x;

    float x = (float)(launchIndex.x + 0.5f) / plp.imageSize.x;
    float y = (float)(launchIndex.y + 0.5f) / plp.imageSize.y;
    float vh = 2 * std::tan(plp.camera.fovY * 0.5f);
    float vw = plp.camera.aspect * vh;

    float3 origin = make_float3(0, 0, 3);
    float3 direction = normalize(make_float3(vw * (x - 0.5f), vh * (0.5f - y), -1));

    PayloadAccessor<SearchRayPayload> payload;
    optixTrace(plp.topGroup, origin, direction, 0.0f, INFINITY, 0.0f, 0xFF, OPTIX_RAY_FLAG_NONE,
               RayType_Search, NumRayTypes, RayType_Search,
               payload[0], payload[1], payload[2]);

    plp.outputBuffer[index] = make_float4(payload.raw.contribution, 1.0f);
}

RT_PROGRAM void __miss__searchRay() {
    PayloadAccessor<SearchRayPayload> payload;

    payload.raw.contribution = make_float3(0.0f, 0.0f, 0.05f);
    
    payload.setAll();
}

RT_PROGRAM void __closesthit__shading() {
    const auto &sbtrData = *(HitGroupData*)optixGetSbtDataPointer();

    auto hitPointParam = HitPointParameter::get();

    PayloadAccessor<SearchRayPayload> payload;

    payload.raw.contribution = sbtrData.mat.albedo;

    //payload.raw.contribution = make_float3(hitPointParam.b0,
    //                                       hitPointParam.b1,
    //                                       0.0f);

    //const Triangle &tri = sbtrData.geom.triangleBuffer[hitPointParam.primIndex];
    //float3 n0 = sbtrData.geom.vertexBuffer[tri.index0].normal;
    //float3 n1 = sbtrData.geom.vertexBuffer[tri.index1].normal;
    //float3 n2 = sbtrData.geom.vertexBuffer[tri.index2].normal;
    //float b0 = hitPointParam.b0;
    //float b1 = hitPointParam.b1;
    //float b2 = 1 - (b0 + b1);
    //float3 sn = normalize(b0 * n0 + b1 * n1 + b2 * n2);
    //payload.raw.contribution = 0.5f * sn + make_float3(0.5f, 0.5f, 0.5f);

    payload.setAll();
}

RT_PROGRAM void __anyhit__visibility() {
    PayloadAccessor<VisibilityRayPayload> payload;

    payload.raw.visibility = 0.0f;
    payload.setAll();

    optixTerminateRay();
}

}
