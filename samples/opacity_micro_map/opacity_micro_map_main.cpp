﻿/*

JP: このサンプルはAny-Hit Program呼び出しを削減することでアルファテストなどを高速化する
    Opacity Micro-Map (OMM)の使用方法を示します。
    OMMは三角形メッシュにおけるテクスチャー等によるジオメトリの切り抜きに関する情報を事前計算したものです。
    GASの生成時に追加情報として渡すことで少量の追加メモリと引き換えにAny-Hit Programの呼び出し回数を削減し、
    アルファテストなどが有効なジオメトリに対するレイトレーシングを高速化することができます。
    OptiXのAPIにはOMM自体の生成処理は含まれていないため、何らかの手段を用いて生成する必要がありますが、
    このサンプルにはOMMの生成処理も含まれています。

EN: This sample shows how to use Opacity Micro-Map (OMM) which accelerates alpha tests, etc. by reducing
    any-hit program calls.
    OMM is precomputed information regarding geometry cutouts by textures or something for triangle mesh.
    Providing OMM as additional information when building a GAS costs a bit of additional memory but
    reduces any-hit program calls to accelerate ray tracing for geometries with alpha tests.
    OptiX API doesn't provide generation of OMM itself, so OMM generation by some means is required.
    This sample also provide OMM generation.

*/

#include "opacity_micro_map_shared.h"

#include "../common/obj_loader.h"
#include "../common/dds_loader.h"
#define STB_IMAGE_IMPLEMENTATION
#include "../../ext/stb_image.h"

#include "omm_generator.h"

int32_t main(int32_t argc, const char* argv[]) try {
    auto visualizationMode = Shared::VisualizationMode_Final;

    uint32_t argIdx = 1;
    while (argIdx < argc) {
        std::string_view arg = argv[argIdx];
        if (arg == "--visualize") {
            if (argIdx + 1 >= argc)
                throw std::runtime_error("Argument for --visualize is not complete.");
            std::string_view visType = argv[argIdx + 1];
            if (visType == "final")
                visualizationMode = Shared::VisualizationMode_Final;
            else if (visType == "primary-any-hits")
                visualizationMode = Shared::VisualizationMode_NumPrimaryAnyHits;
            else if (visType == "shadow-any-hits")
                visualizationMode = Shared::VisualizationMode_NumShadowAnyHits;
            else
                throw std::runtime_error("Argument for --visualize is invalid.");
            argIdx += 1;
        }
        else
            throw std::runtime_error("Unknown command line argument.");
        ++argIdx;
    }

    // ----------------------------------------------------------------
    // JP: OptiXのコンテキストとパイプラインの設定。
    // EN: Settings for OptiX context and pipeline.

    CUcontext cuContext;
    int32_t cuDeviceCount;
    CUstream cuStream;
    CUDADRV_CHECK(cuInit(0));
    CUDADRV_CHECK(cuDeviceGetCount(&cuDeviceCount));
    CUDADRV_CHECK(cuCtxCreate(&cuContext, 0, 0));
    CUDADRV_CHECK(cuCtxSetCurrent(cuContext));
    CUDADRV_CHECK(cuStreamCreate(&cuStream, 0));

    optixu::Context optixContext = optixu::Context::create(cuContext);

    optixu::Pipeline pipeline = optixContext.createPipeline();

    // JP: Opacity Micro-Mapを使う場合、パイプラインオプションで使用を宣言する必要がある。
    // EN: Declaring the use of Opacity micro-map is required in the pipeline option when using it.
    pipeline.setPipelineOptions(
        std::max(Shared::PrimaryRayPayloadSignature::numDwords,
                 Shared::VisibilityRayPayloadSignature::numDwords),
        optixu::calcSumDwords<float2>(),
        "plp", sizeof(Shared::PipelineLaunchParameters),
        OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING,
        OPTIX_EXCEPTION_FLAG_STACK_OVERFLOW | OPTIX_EXCEPTION_FLAG_TRACE_DEPTH |
        DEBUG_SELECT(OPTIX_EXCEPTION_FLAG_DEBUG, OPTIX_EXCEPTION_FLAG_NONE),
        OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE,
        optixu::UseMotionBlur::No, optixu::UseOpacityMicroMaps::Yes);

    const std::vector<char> optixIr =
        readBinaryFile(getExecutableDirectory() / "opacity_micro_map/ptxes/optix_kernels.optixir");
    optixu::Module moduleOptiX = pipeline.createModuleFromOptixIR(
        optixIr, OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT,
        DEBUG_SELECT(OPTIX_COMPILE_OPTIMIZATION_LEVEL_0, OPTIX_COMPILE_OPTIMIZATION_DEFAULT),
        DEBUG_SELECT(OPTIX_COMPILE_DEBUG_LEVEL_FULL, OPTIX_COMPILE_DEBUG_LEVEL_NONE));

    optixu::Module emptyModule;

    optixu::ProgramGroup rayGenProgram =
        pipeline.createRayGenProgram(moduleOptiX, RT_RG_NAME_STR("raygen"));
    //optixu::ProgramGroup exceptionProgram = pipeline.createExceptionProgram(moduleOptiX, "__exception__print");

    optixu::ProgramGroup missProgram = pipeline.createMissProgram(
        moduleOptiX, RT_MS_NAME_STR("miss"));
    optixu::ProgramGroup emptyMissProgram = pipeline.createMissProgram(emptyModule, nullptr);

    optixu::ProgramGroup shadingHitProgramGroup = pipeline.createHitProgramGroupForTriangleIS(
        moduleOptiX, RT_CH_NAME_STR("shading"),
        emptyModule, nullptr);
    optixu::ProgramGroup shadingWithAlphaHitProgramGroup = pipeline.createHitProgramGroupForTriangleIS(
        moduleOptiX, RT_CH_NAME_STR("shading"),
        moduleOptiX, RT_AH_NAME_STR("primary"));
    optixu::ProgramGroup visibilityHitProgramGroup = pipeline.createHitProgramGroupForTriangleIS(
        emptyModule, nullptr,
        moduleOptiX, RT_AH_NAME_STR("visibility"));
    optixu::ProgramGroup visibilityWithAlphaHitProgramGroup = pipeline.createHitProgramGroupForTriangleIS(
        emptyModule, nullptr,
        moduleOptiX, RT_AH_NAME_STR("visibilityWithAlpha"));

    pipeline.link(2, DEBUG_SELECT(OPTIX_COMPILE_DEBUG_LEVEL_FULL, OPTIX_COMPILE_DEBUG_LEVEL_NONE));

    pipeline.setRayGenerationProgram(rayGenProgram);
    // If an exception program is not set but exception flags are set,
    // the default exception program will by provided by OptiX.
    //pipeline.setExceptionProgram(exceptionProgram);
    pipeline.setNumMissRayTypes(Shared::NumRayTypes);
    pipeline.setMissProgram(Shared::RayType_Primary, missProgram);
    pipeline.setMissProgram(Shared::RayType_Visibility, emptyMissProgram);

    cudau::Buffer shaderBindingTable;
    size_t sbtSize;
    pipeline.generateShaderBindingTableLayout(&sbtSize);
    shaderBindingTable.initialize(cuContext, cudau::BufferType::Device, sbtSize, 1);
    shaderBindingTable.setMappedMemoryPersistent(true);
    pipeline.setShaderBindingTable(shaderBindingTable, shaderBindingTable.getMappedPointer());

    // END: Settings for OptiX context and pipeline.
    // ----------------------------------------------------------------



    // ----------------------------------------------------------------
    // JP: マテリアルのセットアップ。
    // EN: Setup materials.

    constexpr bool useBlockCompressedTexture = true;

    optixu::Material defaultMat = optixContext.createMaterial();
    defaultMat.setHitGroup(Shared::RayType_Primary, shadingHitProgramGroup);
    defaultMat.setHitGroup(Shared::RayType_Visibility, visibilityHitProgramGroup);

    optixu::Material alphaTestMat = optixContext.createMaterial();
    alphaTestMat.setHitGroup(Shared::RayType_Primary, shadingWithAlphaHitProgramGroup);
    alphaTestMat.setHitGroup(Shared::RayType_Visibility, visibilityWithAlphaHitProgramGroup);

    // END: Setup materials.
    // ----------------------------------------------------------------



    // ----------------------------------------------------------------
    // JP: シーンのセットアップ。
    // EN: Setup a scene.

    optixu::Scene scene = optixContext.createScene();

    size_t maxSizeOfScratchBuffer = 0;
    OptixAccelBufferSizes asMemReqs;

    cudau::Buffer asBuildScratchMem;

    struct Geometry {
        cudau::TypedBuffer<Shared::Vertex> vertexBuffer;
        struct MaterialGroup {
            cudau::TypedBuffer<Shared::Triangle> triangleBuffer;
            optixu::GeometryInstance optixGeomInst;
            cudau::Array texArray;
            CUtexObject texObj;
        };
        std::vector<MaterialGroup> matGroups;
        optixu::GeometryAccelerationStructure optixGas;
        cudau::Buffer gasMem;
        size_t compactedSize;

        void finalize() {
            gasMem.finalize();
            optixGas.destroy();
            for (auto it = matGroups.rbegin(); it != matGroups.rend(); ++it) {
                if (it->texObj) {
                    CUDADRV_CHECK(cuTexObjectDestroy(it->texObj));
                    it->texArray.finalize();
                }
                it->triangleBuffer.finalize();
                it->optixGeomInst.destroy();
            }
            vertexBuffer.finalize();
        }
    };

    Geometry floor;
    {
        Shared::Vertex vertices[] = {
            // floor
            { make_float3(-100.0f, 0.0f, -100.0f), make_float3(0, 1, 0), make_float2(0, 0) },
            { make_float3(-100.0f, 0.0f, 100.0f), make_float3(0, 1, 0), make_float2(0, 1) },
            { make_float3(100.0f, 0.0f, 100.0f), make_float3(0, 1, 0), make_float2(1, 1) },
            { make_float3(100.0f, 0.0f, -100.0f), make_float3(0, 1, 0), make_float2(1, 0) },
        };

        Shared::Triangle triangles[] = {
            // floor
            { 0, 1, 2 }, { 0, 2, 3 },
        };

        floor.vertexBuffer.initialize(cuContext, cudau::BufferType::Device, vertices, lengthof(vertices));

        floor.optixGas = scene.createGeometryAccelerationStructure();
        floor.optixGas.setConfiguration(
            optixu::ASTradeoff::PreferFastTrace,
            optixu::AllowUpdate::No,
            optixu::AllowCompaction::Yes);
        floor.optixGas.setNumMaterialSets(1);
        floor.optixGas.setNumRayTypes(0, Shared::NumRayTypes);

        Geometry::MaterialGroup group;
        {
            group.triangleBuffer.initialize(cuContext, cudau::BufferType::Device, triangles, lengthof(triangles));

            Shared::GeometryInstanceData geomData = {};
            geomData.vertexBuffer = floor.vertexBuffer.getDevicePointer();
            geomData.triangleBuffer = group.triangleBuffer.getDevicePointer();
            geomData.texture = 0;
            geomData.albedo = float3(0.8f, 0.8f, 0.8f);

            group.optixGeomInst = scene.createGeometryInstance();
            group.optixGeomInst.setVertexBuffer(floor.vertexBuffer);
            group.optixGeomInst.setTriangleBuffer(group.triangleBuffer);
            group.optixGeomInst.setNumMaterials(1, optixu::BufferView());
            group.optixGeomInst.setMaterial(0, 0, defaultMat);
            group.optixGeomInst.setGeometryFlags(0, OPTIX_GEOMETRY_FLAG_NONE);
            group.optixGeomInst.setUserData(geomData);

            floor.optixGas.addChild(group.optixGeomInst);
            floor.matGroups.push_back(std::move(group));
        }

        floor.optixGas.prepareForBuild(&asMemReqs);
        floor.gasMem.initialize(cuContext, cudau::BufferType::Device, asMemReqs.outputSizeInBytes, 1);
        maxSizeOfScratchBuffer = std::max(maxSizeOfScratchBuffer, asMemReqs.tempSizeInBytes);
    }

    Geometry tree;
    {
        std::filesystem::path filePath =
            R"(C:\Users\shocker_0x15\repos\assets\McguireCGArchive\white_oak\white_oak.obj)";
        std::filesystem::path fileDir = filePath.parent_path();

        std::vector<obj::Vertex> vertices;
        std::vector<obj::MaterialGroup> matGroups;
        std::vector<obj::Material> materials;
        obj::load(filePath, &vertices, &matGroups, &materials);

        tree.vertexBuffer.initialize(
            cuContext, cudau::BufferType::Device,
            reinterpret_cast<Shared::Vertex*>(vertices.data()), vertices.size());

        tree.optixGas = scene.createGeometryAccelerationStructure();
        tree.optixGas.setConfiguration(
            optixu::ASTradeoff::PreferFastTrace,
            optixu::AllowUpdate::No,
            optixu::AllowCompaction::Yes);
        tree.optixGas.setNumMaterialSets(1);
        tree.optixGas.setNumRayTypes(0, Shared::NumRayTypes);

        uint32_t maxNumTrianglesPerGroup = 0;
        for (int groupIdx = 0; groupIdx < matGroups.size(); ++groupIdx) {
            const obj::MaterialGroup &srcGroup = matGroups[groupIdx];
            maxNumTrianglesPerGroup = std::max(
                maxNumTrianglesPerGroup,
                static_cast<uint32_t>(srcGroup.triangles.size()));
        }

        cudau::TypedBuffer<uint32_t> transparentCounts(
            cuContext, cudau::BufferType::Device, maxNumTrianglesPerGroup);
        cudau::TypedBuffer<uint32_t> numPixelsValues(
            cuContext, cudau::BufferType::Device, maxNumTrianglesPerGroup);
        cudau::TypedBuffer<uint32_t> numFetchedTriangles(
            cuContext, cudau::BufferType::Device, 1);
        cudau::TypedBuffer<uint32_t> ommFormatCounts(
            cuContext, cudau::BufferType::Device, Shared::NumOMMFormats);

        for (int groupIdx = 0; groupIdx < matGroups.size(); ++groupIdx) {
            const obj::MaterialGroup &srcGroup = matGroups[groupIdx];
            const obj::Material &srcMat = materials[srcGroup.materialIndex];
            const uint32_t numTriangles = srcGroup.triangles.size();

            Geometry::MaterialGroup group;
            group.triangleBuffer.initialize(
                cuContext, cudau::BufferType::Device,
                reinterpret_cast<const Shared::Triangle*>(srcGroup.triangles.data()),
                numTriangles);

            Shared::GeometryInstanceData geomData = {};
            geomData.vertexBuffer = tree.vertexBuffer.getDevicePointer();
            geomData.triangleBuffer = group.triangleBuffer.getDevicePointer();
            geomData.albedo = float3(srcMat.diffuse[0], srcMat.diffuse[1], srcMat.diffuse[2]);
            if (!srcMat.diffuseTexPath.empty()) {
                int32_t width, height, n;
                uint8_t* linearImageData = stbi_load(
                    srcMat.diffuseTexPath.string().c_str(),
                    &width, &height, &n, 4);
                group.texArray.initialize2D(
                    cuContext, cudau::ArrayElementType::UInt8, 4,
                    cudau::ArraySurface::Disable, cudau::ArrayTextureGather::Disable,
                    width, height, 1);
                group.texArray.write<uint8_t>(linearImageData, width * height * 4);
                stbi_image_free(linearImageData);

                cudau::TextureSampler texSampler;
                texSampler.setXyFilterMode(cudau::TextureFilterMode::Linear);
                texSampler.setMipMapFilterMode(cudau::TextureFilterMode::Point);
                texSampler.setReadMode(cudau::TextureReadMode::NormalizedFloat_sRGB);
                texSampler.setWrapMode(0, cudau::TextureWrapMode::Repeat);
                texSampler.setWrapMode(1, cudau::TextureWrapMode::Repeat);
                group.texObj = texSampler.createTextureObject(group.texArray);
                geomData.texture = group.texObj;
            }

            /*
            */
            std::vector<uint32_t> perTriangleStates;
            uint32_t ommFormatCountsOnHost[Shared::NumOMMFormats];
            evaluatePerTriangleStates(
                tree.vertexBuffer, group.triangleBuffer, numTriangles,
                group.texObj, make_uint2(group.texArray.getWidth(), group.texArray.getHeight()), 4, 3,
                transparentCounts, numPixelsValues, numFetchedTriangles, ommFormatCounts,
                &perTriangleStates, ommFormatCountsOnHost);

            std::vector<OptixOpacityMicromapHistogramEntry> ommHistogramEntries(numTriangles);

            bool allOpaque = true;
            for (int i = 0; i < numTriangles; ++i) {
                uint32_t state = perTriangleStates[i];
                if (state != OPTIX_OPACITY_MICROMAP_STATE_OPAQUE) {
                    allOpaque = false;
                    break;
                }
            }

            // JP: すべての三角形がOpaqueならOMMをそもそも使用しない。
            // EN: Don't use OMM if all the triangles are opaque.
            // TODO: すべての三角形がTransparentなケース。
            if (!allOpaque) {
                OptixOpacityMicromapHistogramEntry entries[Shared::NumOMMFormats];
                for (int i = 0; i < Shared::NumOMMFormats; ++i) {
                    OptixOpacityMicromapHistogramEntry &entry = entries[i];
                    entry.count = ommFormatCountsOnHost[i];
                    entry.format = i == 0 ?
                        OPTIX_OPACITY_MICROMAP_FORMAT_NONE :
                        OPTIX_OPACITY_MICROMAP_FORMAT_4_STATE;
                    entry.subdivisionLevel = i;
                }

                optixu::OpacityMicroMapArray ommArray = scene.createOpacityMicroMapArray();

                OptixMicromapBufferSizes ommArraySizes;
                ommArray.setConfiguration(OPTIX_OPACITY_MICROMAP_FLAG_PREFER_FAST_TRACE);
                ommArray.prepareForBuild(
                    optixu::BufferView(), optixu::BufferView(),
                    entries, Shared::NumOMMFormats, &ommArraySizes);
            }

            /*
            JP: 処理によっては完全にOpaqueなジオメトリはAny-Hit呼び出しが起こらないように
                ジオメトリフラグを設定しても良いかもしれない。
                このサンプルではOpaquenessに関わらずシャドウレイの処理にAny-Hitを使っているため無効化できない。
            EN: 
            */
            group.optixGeomInst = scene.createGeometryInstance();
            group.optixGeomInst.setVertexBuffer(tree.vertexBuffer);
            group.optixGeomInst.setTriangleBuffer(group.triangleBuffer);
            group.optixGeomInst.setNumMaterials(1, optixu::BufferView());
            group.optixGeomInst.setMaterial(0, 0, alphaTestMat);
            group.optixGeomInst.setGeometryFlags(0, OPTIX_GEOMETRY_FLAG_NONE);
            group.optixGeomInst.setUserData(geomData);

            tree.optixGas.addChild(group.optixGeomInst);
            tree.matGroups.push_back(std::move(group));
        }

        ommFormatCounts.finalize();
        numFetchedTriangles.finalize();
        numPixelsValues.finalize();
        transparentCounts.finalize();

        tree.optixGas.prepareForBuild(&asMemReqs);
        tree.gasMem.initialize(cuContext, cudau::BufferType::Device, asMemReqs.outputSizeInBytes, 1);
        maxSizeOfScratchBuffer = std::max(maxSizeOfScratchBuffer, asMemReqs.tempSizeInBytes);
    }



    // JP: GASを基にインスタンスを作成する。
    // EN: Create instances based on GASs.
    optixu::Instance floorInst = scene.createInstance();
    floorInst.setChild(floor.optixGas);

    std::vector<optixu::Instance> treeInsts;
    std::mt19937 treeRng(471203123);
    std::uniform_real_distribution<float> treeU01;
    constexpr float treeScale = 0.003f;
    constexpr uint32_t treeGridSize = 100;
    for (int instIdx = 0; instIdx < treeGridSize * treeGridSize; ++instIdx) {
        optixu::Instance inst = scene.createInstance();
        int32_t iz = instIdx / treeGridSize;
        int32_t ix = instIdx % treeGridSize;
        float dz = 0.5f * (treeU01(treeRng) - 0.5f);
        float dx = 0.5f * (treeU01(treeRng) - 0.5f);
        float z = -100 + (iz + 0.5f + dz) / treeGridSize * 200;
        float x = -100 + (ix + 0.5f + dx) / treeGridSize * 200;
        Matrix3x3 m = rotateY3x3(2 * M_PI * treeU01(treeRng)) * scale3x3(treeScale);
        inst.setChild(tree.optixGas);
        float xfm[] = {
            m.m00, m.m01, m.m02, x,
            m.m10, m.m11, m.m12, 0,
            m.m20, m.m21, m.m22, z,
        };
        inst.setTransform(xfm);
        treeInsts.push_back(inst);
    }



    // JP: Instance Acceleration Structureを生成する。
    // EN: Create an instance acceleration structure.
    optixu::InstanceAccelerationStructure ias = scene.createInstanceAccelerationStructure();
    cudau::Buffer iasMem;
    cudau::TypedBuffer<OptixInstance> instanceBuffer;
    ias.setConfiguration(optixu::ASTradeoff::PreferFastTrace);
    ias.addChild(floorInst);
    for (int i = 0; i < treeInsts.size(); ++i)
        ias.addChild(treeInsts[i]);
    ias.prepareForBuild(&asMemReqs);
    iasMem.initialize(cuContext, cudau::BufferType::Device, asMemReqs.outputSizeInBytes, 1);
    instanceBuffer.initialize(cuContext, cudau::BufferType::Device, ias.getNumChildren());
    maxSizeOfScratchBuffer = std::max(maxSizeOfScratchBuffer, asMemReqs.tempSizeInBytes);



    // JP: ASビルド用のスクラッチメモリを確保する。
    // EN: Allocate scratch memory for AS builds.
    asBuildScratchMem.initialize(cuContext, cudau::BufferType::Device, maxSizeOfScratchBuffer, 1);



    // JP: Geometry Acceleration Structureをビルドする。
    // EN: Build geometry acceleration structures.
    floor.optixGas.rebuild(cuStream, floor.gasMem, asBuildScratchMem);
    tree.optixGas.rebuild(cuStream, tree.gasMem, asBuildScratchMem);

    // JP: 静的なメッシュはコンパクションもしておく。
    //     複数のメッシュのASをひとつのバッファーに詰めて記録する。
    // EN: Perform compaction for static meshes.
    //     Record ASs of multiple meshes into single buffer back to back.
    struct CompactedASInfo {
        Geometry* geom;
        size_t offset;
        size_t size;
    };
    CompactedASInfo gasList[] = {
        { &floor, 0, 0 },
        { &tree, 0, 0 },
    };
    size_t compactedASMemOffset = 0;
    for (int i = 0; i < lengthof(gasList); ++i) {
        CompactedASInfo &info = gasList[i];
        compactedASMemOffset = alignUp(compactedASMemOffset, OPTIX_ACCEL_BUFFER_BYTE_ALIGNMENT);
        info.offset = compactedASMemOffset;
        info.geom->optixGas.prepareForCompact(&info.size);
        compactedASMemOffset += info.size;
    }
    cudau::Buffer compactedASMem;
    compactedASMem.initialize(cuContext, cudau::BufferType::Device, compactedASMemOffset, 1);
    for (int i = 0; i < lengthof(gasList); ++i) {
        const CompactedASInfo &info = gasList[i];
        info.geom->optixGas.compact(
            cuStream,
            optixu::BufferView(compactedASMem.getCUdeviceptr() + info.offset, info.size, 1));
    }
    // JP: removeUncompacted()はcompact()がデバイス上で完了するまでホスト側で待つので呼び出しを分けたほうが良い。
    // EN: removeUncompacted() waits on host-side until the compact() completes on the device,
    //     so separating calls is recommended.
    for (int i = 0; i < lengthof(gasList); ++i) {
        gasList[i].geom->optixGas.removeUncompacted();
        gasList[i].geom->gasMem.finalize();
    }



    // JP: IASビルド時には各インスタンスのTraversable HandleとShader Binding Table中のオフセットが
    //     確定している必要がある。
    // EN: Traversable handle and offset in the shader binding table must be fixed for each instance
    //     when building an IAS.
    cudau::Buffer hitGroupSBT;
    size_t hitGroupSbtSize;
    scene.generateShaderBindingTableLayout(&hitGroupSbtSize);
    hitGroupSBT.initialize(cuContext, cudau::BufferType::Device, hitGroupSbtSize, 1);
    hitGroupSBT.setMappedMemoryPersistent(true);

    OptixTraversableHandle travHandle = ias.rebuild(cuStream, instanceBuffer, iasMem, asBuildScratchMem);

    CUDADRV_CHECK(cuStreamSynchronize(cuStream));

    // END: Setup a scene.
    // ----------------------------------------------------------------



    constexpr uint32_t renderTargetSizeX = 1280;
    constexpr uint32_t renderTargetSizeY = 720;
    cudau::Array colorAccumBuffer;
    colorAccumBuffer.initialize2D(
        cuContext, cudau::ArrayElementType::Float32, 4,
        cudau::ArraySurface::Enable, cudau::ArrayTextureGather::Disable,
        renderTargetSizeX, renderTargetSizeY, 1);


    
    Shared::PipelineLaunchParameters plp;
    plp.travHandle = travHandle;
    plp.imageSize = int2(renderTargetSizeX, renderTargetSizeY);
    plp.colorAccumBuffer = colorAccumBuffer.getSurfaceObject(0);
    plp.camera.fovY = 50 * pi_v<float> / 180;
    plp.camera.aspect = static_cast<float>(renderTargetSizeX) / renderTargetSizeY;
    plp.camera.position = make_float3(0, 2, 5);
    plp.camera.orientation = rotateY3x3(0.8f * pi_v<float>) * rotateX3x3(pi_v<float> / 12);
    plp.lightDirection = normalize(float3(1, 5, 2));
    plp.lightRadiance = float3(7.5f, 7.5f, 7.5f);
    plp.envRadiance = float3(0.10f, 0.13f, 0.9f);
    plp.visualizationMode = visualizationMode;

    pipeline.setScene(scene);
    pipeline.setHitGroupShaderBindingTable(hitGroupSBT, hitGroupSBT.getMappedPointer());

    CUdeviceptr plpOnDevice;
    CUDADRV_CHECK(cuMemAlloc(&plpOnDevice, sizeof(plp)));



    cudau::Timer timerRender;
    timerRender.initialize(cuContext);
    
    // JP: レンダリング
    // EN: Render
    timerRender.start(cuStream);
    const uint32_t superSampleSize = 8;
    plp.superSampleSizeMinus1 = superSampleSize - 1;
    for (int frameIndex = 0; frameIndex < superSampleSize * superSampleSize; ++frameIndex) {
        plp.sampleIndex = frameIndex;
        CUDADRV_CHECK(cuMemcpyHtoDAsync(plpOnDevice, &plp, sizeof(plp), cuStream));
        pipeline.launch(cuStream, plpOnDevice, renderTargetSizeX, renderTargetSizeY, 1);
    }
    timerRender.stop(cuStream);

    CUDADRV_CHECK(cuStreamSynchronize(cuStream));

    float renderTime = timerRender.report();
    hpprintf("Render: %.3f[ms]\n", renderTime);

    timerRender.finalize();

    // JP: 結果の画像出力。
    // EN: Output the result as an image.
    saveImage("output.png", colorAccumBuffer, true, true);



    CUDADRV_CHECK(cuMemFree(plpOnDevice));


    
    colorAccumBuffer.finalize();



    hitGroupSBT.finalize();

    compactedASMem.finalize();

    asBuildScratchMem.finalize();

    instanceBuffer.finalize();
    iasMem.finalize();
    ias.destroy();

    for (int i = 0; i < treeInsts.size(); ++i)
        treeInsts[i].destroy();
    floorInst.destroy();

    tree.finalize();
    floor.finalize();

    scene.destroy();

    alphaTestMat.destroy();
    defaultMat.destroy();



    shaderBindingTable.finalize();

    visibilityWithAlphaHitProgramGroup.destroy();
    visibilityHitProgramGroup.destroy();
    shadingWithAlphaHitProgramGroup.destroy();
    shadingHitProgramGroup.destroy();

    emptyMissProgram.destroy();
    missProgram.destroy();
    rayGenProgram.destroy();

    moduleOptiX.destroy();

    pipeline.destroy();

    optixContext.destroy();

    CUDADRV_CHECK(cuStreamDestroy(cuStream));
    CUDADRV_CHECK(cuCtxDestroy(cuContext));

    return 0;
}
catch (const std::exception &ex) {
    hpprintf("Error: %s\n", ex.what());
    return -1;
}
