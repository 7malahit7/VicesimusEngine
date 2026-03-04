#include <loader.h>

#include <rendering/renderer.h>
#include <util.h>
#include <resources/animation.h>
#include <components/bone.h>
#include <components/skeleton.h>
#include <components/mesh_instance.h>
#include <components/point_light.h>
#include <components/skinned_mesh.h>
#include <components/physics/static_body.h>
#include <core/node.h>
#include <core/resource_manager.h>
#include <resources/mesh.h>
#include <resources/texture.h>

#include <stb_image.h>
#include <iostream>
#include <fstream>
#include <span>
#include <string>

#include <variant>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/glm_element_traits.hpp>

#include <ktxvulkan.h>
#include <yaml.h>

#include <Jolt/Jolt.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
using namespace vk;

extern Renderer gRenderer;

static Filter extract_filter(fastgltf::Filter filter) {
    switch(filter) {
        case fastgltf::Filter::Nearest:
        case fastgltf::Filter::NearestMipMapNearest:
        case fastgltf::Filter::NearestMipMapLinear:
            return Filter::eNearest;

        case fastgltf::Filter::Linear:
        case fastgltf::Filter::LinearMipMapNearest:
        case fastgltf::Filter::LinearMipMapLinear:
        default:
            return Filter::eLinear;
    }
}

static SamplerAddressMode extract_address_mode(fastgltf::Wrap wrap) {
    switch (wrap) {
        case fastgltf::Wrap::ClampToEdge:     return SamplerAddressMode::eClampToEdge;
        case fastgltf::Wrap::MirroredRepeat:  return SamplerAddressMode::eMirroredRepeat;
        case fastgltf::Wrap::Repeat:         return SamplerAddressMode::eRepeat;
    }
    return SamplerAddressMode::eRepeat;
}

static SamplerMipmapMode extract_mipmap_mode(fastgltf::Filter filter) {
    switch (filter) {
        case fastgltf::Filter::NearestMipMapNearest:
        case fastgltf::Filter::LinearMipMapNearest:
            return SamplerMipmapMode::eNearest;

        case fastgltf::Filter::NearestMipMapLinear:
        case fastgltf::Filter::LinearMipMapLinear:
        default:
            return SamplerMipmapMode::eLinear;
    }
}

std::optional<AllocatedImage> load_image(Renderer* p_renderer, std::filesystem::path p_file_path, Format p_format) {
    AllocatedImage new_image {};
    if (p_file_path.extension() == ".ktx2") {
        ktxTexture2* k_texture;
        auto result = ktxTexture2_CreateFromNamedFile(p_file_path.c_str(),
                                                     KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                                     &k_texture);
        if (result != KTX_SUCCESS) {
            print("Could not create KTX texture!");
        }

        auto color_model = ktxTexture2_GetColorModel_e(k_texture);
        PhysicalDeviceFeatures features = p_renderer->physical_device.getFeatures();
        if (color_model != KHR_DF_MODEL_RGBSDA) {
            ktx_transcode_fmt_e texture_format;
            if (color_model == KHR_DF_MODEL_UASTC && features.textureCompressionASTC_LDR) {
                texture_format = KTX_TTF_ASTC_4x4_RGBA;
            } else if (color_model == KHR_DF_MODEL_ETC1S && features.textureCompressionETC2) {
                texture_format = KTX_TTF_ETC;
            } else if (features.textureCompressionASTC_LDR) {
                texture_format = KTX_TTF_ASTC_4x4_RGBA;
            } else if (features.textureCompressionETC2) {
                texture_format = KTX_TTF_ETC2_RGBA;
            } else if (features.textureCompressionBC) {
                texture_format = KTX_TTF_BC3_RGBA;
            } else {
                print("Could not transcode texture!");
                return {};
            }
            (void)texture_format;
            result = ktxTexture2_TranscodeBasis(k_texture, KTX_TTF_BC7_RGBA, 0);
            if (result != KTX_SUCCESS) {
                print("Could not transcode texture!");
            }
        }

        ktxVulkanTexture texture;
        result = ktxTexture2_VkUploadEx(k_texture, &p_renderer->ktx_device_info, &texture,
                                        VK_IMAGE_TILING_OPTIMAL,
                                        VK_IMAGE_USAGE_SAMPLED_BIT,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (result != KTX_SUCCESS) {
            print("Could not upload KTX texture!");
        }

        new_image.ktx_texture = texture;
        new_image.image = texture.image;

        ImageViewCreateInfo view_info {
            .image    = texture.image,
            .viewType = static_cast<ImageViewType>(texture.viewType),
            .format   = static_cast<Format>(texture.imageFormat),
            .subresourceRange = ImageSubresourceRange {
                .aspectMask     = ImageAspectFlagBits::eColor,
                .baseMipLevel   = 0,
                .levelCount     = texture.levelCount,
                .baseArrayLayer = 0,
                .layerCount     = texture.layerCount,
            }
        };

        Result view_result;
        std::tie(view_result, new_image.image_view) = p_renderer->device.createImageView(view_info);
        (void)view_result;

        ktxTexture2_Destroy(k_texture);
        return new_image;
    }

    int w, h, nrChannels;
    unsigned char* data = stbi_load(p_file_path.c_str(), &w, &h, &nrChannels, 4);
    new_image = p_renderer->create_image(data, Extent3D {uint32_t(w), uint32_t(h), 1}, p_format,
                                         ImageUsageFlagBits::eSampled, true);
    return new_image;
}

std::optional<AllocatedImage> load_image(Renderer* p_renderer, fastgltf::Asset& p_asset, fastgltf::Image& p_image, Format p_format) {
    AllocatedImage new_image {};
    int width, height, number_channels;
    std::visit(
        fastgltf::visitor {
            [](auto&) {},
            [&](fastgltf::sources::URI& file_path) {
                assert(file_path.fileByteOffset == 0);
                if (!file_path.uri.isLocalPath()) {
                    return;
                }
                auto full_path = std::string("assets/models/Sponza/glTF/") + file_path.uri.c_str();
                unsigned char* data = stbi_load(full_path.c_str(), &width, &height, &number_channels, 4);
                if (data) {
                    Extent3D image_size { uint32_t(width), uint32_t(height), 1 };
                    new_image = p_renderer->create_image(data, image_size, p_format, ImageUsageFlagBits::eSampled, true);
                    stbi_image_free(data);
                }
            },
            [&](fastgltf::sources::Array& array) {
                unsigned char* data = stbi_load_from_memory((stbi_uc*)array.bytes.data(),
                                                           static_cast<int>(array.bytes.size()),
                                                           &width, &height, &number_channels, 4);
                if (data) {
                    Extent3D image_size { uint32_t(width), uint32_t(height), 1 };
                    new_image = p_renderer->create_image(data, image_size, p_format, ImageUsageFlagBits::eSampled, true);
                    stbi_image_free(data);
                }
            },
            [&](fastgltf::sources::BufferView& view) {
                auto& buffer_view = p_asset.bufferViews[view.bufferViewIndex];
                auto& buffer = p_asset.buffers[buffer_view.bufferIndex];
                std::visit(
                    fastgltf::visitor {
                        [](auto&) {},
                        [&](fastgltf::sources::Array& array) {
                            unsigned char* data = stbi_load_from_memory((stbi_uc*)array.bytes.data() + buffer_view.byteOffset,
                                                                       static_cast<int>(buffer_view.byteLength),
                                                                       &width, &height, &number_channels, 4);
                            if (data) {
                                Extent3D image_size { uint32_t(width), uint32_t(height), 1 };
                                new_image = p_renderer->create_image(data, image_size, p_format, ImageUsageFlagBits::eSampled, true);
                                stbi_image_free(data);
                            }
                        }
                    }, buffer.data
                );
            },
        }, p_image.data
    );

    if (new_image.image == VK_NULL_HANDLE) return {};
    return new_image;
}

void import_gltf_scene(Renderer* p_renderer, std::filesystem::path p_file_path) {
    print("Importing GLTF: %s", p_file_path.c_str());

    auto target_path = std::format("{}.imported", p_file_path.c_str());
    std::fstream file(target_path.c_str(), std::fstream::out | std::fstream::binary);
    if (!file.is_open()) {
        print("Error: cannot open for write: %s", target_path.c_str());
        return;
    }

    constexpr auto gltf_options =
        fastgltf::Options::DontRequireValidAssetMember |
        fastgltf::Options::AllowDouble |
        fastgltf::Options::LoadExternalBuffers;

    auto data = fastgltf::GltfDataBuffer::FromPath(p_file_path);
    if (data.error() != fastgltf::Error::None) {
        print("Could not load GLTF!");
        return;
    }

    YAML::Node import_settings;
    std::string import_settings_path = std::format("{}.import", p_file_path.c_str());
    if (std::filesystem::exists(import_settings_path)) {
        import_settings = YAML::LoadFile(import_settings_path);
    }

    fastgltf::Asset asset;
    fastgltf::Parser parser {};
    auto type = fastgltf::determineGltfFileType(data.get());

    if (type == fastgltf::GltfType::glTF) {
        auto load = parser.loadGltf(data.get(), p_file_path.parent_path(), gltf_options);
        if (load.error() != fastgltf::Error::None) {
            print("Could not parse GLTF!");
            return;
        }
        asset = std::move(load.get());
    } else if (type == fastgltf::GltfType::GLB) {
        auto load = parser.loadGltfBinary(data.get(), p_file_path.parent_path(), gltf_options);
        if (load.error() != fastgltf::Error::None) {
            print("Could not parse GLTF!");
            return;
        }
        asset = std::move(load.get());
    } else {
        print("Could not parse GLTF!");
        return;
    }





    std::vector<SamplerCreateInfo> sampler_infos;
    sampler_infos.reserve(asset.samplers.size());
    for (fastgltf::Sampler& sampler : asset.samplers) {
        sampler_infos.emplace_back(SamplerCreateInfo{
            .magFilter    = extract_filter(sampler.magFilter.value_or(fastgltf::Filter::Nearest)),
            .minFilter    = extract_filter(sampler.minFilter.value_or(fastgltf::Filter::Nearest)),
            .mipmapMode   = extract_mipmap_mode(sampler.minFilter.value_or(fastgltf::Filter::Nearest)),
            .addressModeU = extract_address_mode(sampler.wrapS),
            .addressModeV = extract_address_mode(sampler.wrapT),
            .minLod       = 0,
            .maxLod       = LodClampNone,
        });
    }

    file << "Samplers\n";
    file << sampler_infos.size() << "\n";
    if (!sampler_infos.empty()) {
        file.write(reinterpret_cast<char*>(sampler_infos.data()), sampler_infos.size() * sizeof(SamplerCreateInfo));
    }

    // Textures
    file << "Textures\n";
    file << asset.images.size() << "\n";

    for (auto& image : asset.images) {
        std::visit(
            fastgltf::visitor{
                [](auto&) {},
                [&](fastgltf::sources::URI& file_path) {
                    assert(file_path.fileByteOffset == 0);

                    if (!file_path.uri.isLocalPath()) {
                        file << "-1\n";
                        return;
                    }

                    auto full_path = (p_file_path.parent_path() / file_path.uri.c_str()).string();
                    auto ktx_path = std::format("{}.{}", full_path, "ktx2");
                    if (std::filesystem::exists(ktx_path)) {
                        full_path = ktx_path;
                        file << full_path << "\n";
                        return;
                    }

                    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
                    bool format_specified = false;
                    if (import_settings["Textures"]) {
                        for (auto texture : import_settings["Textures"]) {
                            if (texture["name"] && texture["name"].as<std::string>() == image.name.c_str()) {
                                format = texture["sRGB"].as<bool>() ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
                                format_specified = true;
                            }
                        }
                    }
                    if (!format_specified) {
                        YAML::Node texture_node {};
                        texture_node["name"] = image.name.c_str();
                        texture_node["sRGB"] = false;
                        import_settings["Textures"].push_back(texture_node);
                    }

                    int width, height, number_channels;
                    unsigned char* data = stbi_load(full_path.c_str(), &width, &height, &number_channels, 4);
                    if (data == nullptr) {
                        print("WARN: could not load texture file: %s", full_path.c_str());
                        file << "-1\n";
                        return;
                    }

                    ktxTexture2* texture = nullptr;
                    ktxTextureCreateInfo ktx_info {
                        .vkFormat        = format,
                        .baseWidth       = (ktx_uint32_t)width,
                        .baseHeight      = (ktx_uint32_t)height,
                        .baseDepth       = 1,
                        .numDimensions   = 2,
                        .numLevels       = 1,
                        .numLayers       = 1,
                        .numFaces        = 1,
                        .isArray         = KTX_FALSE,
                        .generateMipmaps = KTX_TRUE,
                    };

                    auto result = ktxTexture2_Create(&ktx_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
                    if (result != KTX_SUCCESS || !texture) {
                        print("WARN: ktxTexture2_Create failed");
                        stbi_image_free(data);
                        file << "-1\n";
                        return;
                    }

                    texture->pData = static_cast<uint8_t*>(data);
                    auto texture_path = std::format("{}/{}.ktx2", p_file_path.parent_path().c_str(), image.name.c_str());
                    ktxTexture_WriteToNamedFile(ktxTexture(texture), texture_path.c_str());

                    texture->pData = nullptr;
                    ktxTexture_Destroy(ktxTexture(texture));
                    stbi_image_free(data);

                    file << texture_path << "\n";
                },
                [&](fastgltf::sources::Array&) {
                    file << "-1\n";
                },
                [&](fastgltf::sources::BufferView& view) {
                    auto& buffer_view = asset.bufferViews[view.bufferViewIndex];
                    auto& buffer = asset.buffers[buffer_view.bufferIndex];
                    std::visit(
                        fastgltf::visitor{
                            [](auto&) {},
                            [&](fastgltf::sources::Array& array) {
                                VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
                                bool format_specified = false;
                                if (import_settings["Textures"]) {
                                    for (auto texture : import_settings["Textures"]) {
                                        if (texture["name"] && texture["name"].as<std::string>() == image.name.c_str()) {
                                            format = texture["sRGB"].as<bool>() ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
                                            format_specified = true;
                                        }
                                    }
                                }
                                if (!format_specified) {
                                    YAML::Node texture_node {};
                                    texture_node["name"] = image.name.c_str();
                                    texture_node["sRGB"] = false;
                                    import_settings["Textures"].push_back(texture_node);
                                }

                                int width, height, number_channels;
                                unsigned char* data = stbi_load_from_memory(
                                    (stbi_uc*)array.bytes.data() + buffer_view.byteOffset,
                                    static_cast<int>(buffer_view.byteLength),
                                    &width, &height, &number_channels, 4
                                );
                                if (data == nullptr) {
                                    file << "-1\n";
                                    return;
                                }

                                ktxTexture2* texture = nullptr;
                                ktxTextureCreateInfo ktx_info {
                                    .vkFormat        = format,
                                    .baseWidth       = (ktx_uint32_t)width,
                                    .baseHeight      = (ktx_uint32_t)height,
                                    .baseDepth       = 1,
                                    .numDimensions   = 2,
                                    .numLevels       = 1,
                                    .numLayers       = 1,
                                    .numFaces        = 1,
                                    .isArray         = KTX_FALSE,
                                    .generateMipmaps = KTX_TRUE,
                                };

                                auto result = ktxTexture2_Create(&ktx_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
                                if (result != KTX_SUCCESS || !texture) {
                                    stbi_image_free(data);
                                    file << "-1\n";
                                    return;
                                }

                                texture->pData = static_cast<uint8_t*>(data);
                                auto texture_path = std::format("{}/{}.ktx2", p_file_path.parent_path().c_str(), image.name.c_str());
                                ktxTexture_WriteToNamedFile(ktxTexture(texture), texture_path.c_str());

                                texture->pData = nullptr;
                                ktxTexture_Destroy(ktxTexture(texture));
                                stbi_image_free(data);

                                file << texture_path << "\n";
                            }
                        }, buffer.data
                    );
                },
            }, image.data
        );
    }

    // Materials
    file << "Materials\n";
    file << asset.materials.size() << "\n";

    for (auto& m : asset.materials) {
        file << (float)m.pbrData.baseColorFactor[0] << "\n";
        file << (float)m.pbrData.baseColorFactor[1] << "\n";
        file << (float)m.pbrData.baseColorFactor[2] << "\n";
        file << (float)m.pbrData.baseColorFactor[3] << "\n";
        file << (float)m.pbrData.metallicFactor << "\n";
        file << (float)m.pbrData.roughnessFactor << "\n";

        if (m.pbrData.baseColorTexture.has_value()) {
            size_t texIndex = m.pbrData.baseColorTexture->textureIndex;
            size_t image_index   = asset.textures[texIndex].imageIndex.value();
            size_t sampler_index = asset.textures[texIndex].samplerIndex.value();
            file << image_index << "\n";
            file << sampler_index << "\n";
        } else {
            file << -1 << "\n" << -1 << "\n";
        }

        if (m.normalTexture.has_value()) {
            size_t texIndex = m.normalTexture->textureIndex;
            size_t image_index   = asset.textures[texIndex].imageIndex.value();
            size_t sampler_index = asset.textures[texIndex].samplerIndex.value();
            file << image_index << "\n";
            file << sampler_index << "\n";
        } else {
            file << -1 << "\n" << -1 << "\n";
        }

        if (m.pbrData.metallicRoughnessTexture.has_value()) {
            size_t texIndex = m.pbrData.metallicRoughnessTexture->textureIndex;
            size_t image_index   = asset.textures[texIndex].imageIndex.value();
            size_t sampler_index = asset.textures[texIndex].samplerIndex.value();
            file << image_index << "\n";
            file << sampler_index << "\n";
        } else {
            file << -1 << "\n" << -1 << "\n";
        }
    }

    // Skin
    std::vector<JointData> joint_data;
    if (!asset.skins.empty()) {
        auto inverse_bind_matrix_accessor_index = asset.skins[0].inverseBindMatrices;
        auto inverse_bind_matrix_accessor = asset.accessors[inverse_bind_matrix_accessor_index.value()];
        joint_data.resize(inverse_bind_matrix_accessor.count);

        fastgltf::iterateAccessorWithIndex<Mat4>(asset, inverse_bind_matrix_accessor,
            [&](Mat4 inverse_bind_matrix, size_t index) {
                auto node_index = asset.skins[0].joints[index];
                joint_data[node_index].inverse_bind_matrix = inverse_bind_matrix;
                joint_data[index].parent_index = -1;
            }
        );

        for (uint32_t joint_index = 0; joint_index < asset.skins[0].joints.size(); joint_index++) {
            auto node_index = asset.skins[0].joints[joint_index];
            auto joint = asset.nodes[node_index];

            if (std::holds_alternative<fastgltf::TRS>(joint.transform)) {
                auto trs = std::get<fastgltf::TRS>(joint.transform);
                joint_data[node_index].position = Vec3(trs.translation[0], trs.translation[1], trs.translation[2]);
                joint_data[node_index].rotation = glm::quat(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
                joint_data[node_index].scale    = trs.scale[0]; // NOTE: у тебя тут scalar — оставил как было
            } else {
                joint_data[node_index].position = Vec3(0.0f);
                joint_data[node_index].rotation = glm::quat(1,0,0,0);
                joint_data[node_index].scale    = 1.0f;
            }

            for (auto child_index : joint.children) {
                joint_data[child_index].parent_index = (int)node_index;
            }
        }
    }

    // Animations
    file << "Animations\n";
    file << asset.animations.size() << "\n";

    if (!asset.animations.empty()) {
        for (size_t ai = 0; ai < asset.animations.size(); ++ai) {
    		auto& gltf_animation = asset.animations[ai];
    		std::string anim_name = gltf_animation.name.c_str();
    		if (anim_name.empty()) anim_name = std::format("anim_{}", ai);
    		file << anim_name << "\n";
			print("IMPORT: anim[%zu] name='%s' channels=%zu", ai, anim_name.c_str(), gltf_animation.channels.size());

            float animation_length = 0.0f;
            std::unordered_map<uint32_t, AnimationChannel> channels;

            for (auto gltf_channel : gltf_animation.channels) {
                if (!gltf_channel.nodeIndex.has_value()) continue;

                auto animation_sampler = gltf_animation.samplers[gltf_channel.samplerIndex];
                auto input_accessor = asset.accessors[animation_sampler.inputAccessor];
                auto output_accessor = asset.accessors[animation_sampler.outputAccessor];

                uint32_t node_index = (uint32_t)gltf_channel.nodeIndex.value();
                if (channels.find(node_index) == channels.end()) {
                    channels[node_index] = AnimationChannel{};
                }
                auto* channel = &channels[node_index];

                if (gltf_channel.path == fastgltf::AnimationPath::Translation) {
                    fastgltf::iterateAccessorWithIndex<float>(asset, input_accessor,
                        [&](float p_time, size_t) {
                            animation_length = std::max(animation_length, p_time);
                            KeyframePosition keyframe{};
                            keyframe.time = p_time;
                            channel->position_keyframes.push_back(keyframe);
                        }
                    );
                    fastgltf::iterateAccessorWithIndex<Vec3>(asset, output_accessor,
                        [&](Vec3 p_position, size_t index) {
                            if (index < channel->position_keyframes.size()) {
                                channel->position_keyframes[index].position = p_position;
                            }
                        }
                    );
                } else if (gltf_channel.path == fastgltf::AnimationPath::Rotation) {
                    fastgltf::iterateAccessorWithIndex<float>(asset, input_accessor,
                        [&](float p_time, size_t) {
                            animation_length = std::max(animation_length, p_time);
                            KeyframeRotation keyframe{};
                            keyframe.time = p_time;
                            channel->rotation_keyframes.push_back(keyframe);
                        }
                    );
                    fastgltf::iterateAccessorWithIndex<Vec4>(asset, output_accessor,
                        [&](Vec4 p_rotation, size_t index) {
                            if (index < channel->rotation_keyframes.size()) {
                                channel->rotation_keyframes[index].rotation =
                                    Quaternion(p_rotation[3], p_rotation[0], p_rotation[1], p_rotation[2]);
                            }
                        }
                    );
                }
            }

            file << animation_length << "\n";
            file << channels.size() << "\n";

            for (auto& kv : channels) {
                file << kv.first << "\n"; // node index

                file << kv.second.position_keyframes.size() << "\n";
                for (auto& keyframe : kv.second.position_keyframes) {
                    file << keyframe.time << "\n";
                    file << keyframe.position.x << "\n";
                    file << keyframe.position.y << "\n";
                    file << keyframe.position.z << "\n";
                }

                file << kv.second.rotation_keyframes.size() << "\n";
                for (auto& keyframe : kv.second.rotation_keyframes) {
                    file << keyframe.time << "\n";
                    file << keyframe.rotation.x << "\n";
                    file << keyframe.rotation.y << "\n";
                    file << keyframe.rotation.z << "\n";
                    file << keyframe.rotation.w << "\n";
                }
            }
        }
    }



    // Meshes
    std::vector<uint32_t> indices;
    std::vector<Vertex> vertices;
    std::vector<SkinningData> skinning_data;

    file << "Meshes\n";
    file << asset.meshes.size() << "\n";

    for (auto& mesh : asset.meshes) {
        file << "Mesh\n";
        file << mesh.name.c_str() << "\n";

        indices.clear();
        vertices.clear();
        skinning_data.clear();

        auto mesh_name = mesh.name;
        bool generate_collision_shape = mesh_name.ends_with("_col");

        for (auto&& primitive : mesh.primitives) {
            MeshSurface new_surface{};
            new_surface.start_index = (uint32_t)indices.size();
            new_surface.count = (uint32_t)asset.accessors[primitive.indicesAccessor.value()].count;

            size_t initial_vertex_index = vertices.size();

            // Indices
            {
                fastgltf::Accessor& index_accessor = asset.accessors[primitive.indicesAccessor.value()];
                indices.reserve(indices.size() + index_accessor.count);
                fastgltf::iterateAccessor<std::uint32_t>(asset, index_accessor,
                    [&](std::uint32_t index) { indices.push_back((uint32_t)initial_vertex_index + index); }
                );
            }

            // Positions
            {
                fastgltf::Accessor& position_accessor = asset.accessors[primitive.findAttribute("POSITION")->accessorIndex];
                vertices.resize(vertices.size() + position_accessor.count);

				bool has_uv = primitive.findAttribute("TEXCOORD_0") != primitive.attributes.end();

fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, position_accessor,
[&](glm::vec3 vector, size_t index) {

    Vertex vert{
        .position = vector,
        .uv_x = vector.x * 0.1f,
        .normal = {1.0f, 0.0f, 0.0f},
        .uv_y = vector.z * 0.1f,
        .color = Vec4(1.0f),
    };

    vertices[initial_vertex_index + index] = vert;
});
}

            // Normals
            auto normals = primitive.findAttribute("NORMAL");
            if (normals != primitive.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<Vec3>(asset, asset.accessors[(*normals).accessorIndex],
                    [&](Vec3 normal, size_t index) {
                        vertices[initial_vertex_index + index].normal = glm::normalize(normal);
                    }
                );
            }

            // Tangents
            auto tangents = primitive.findAttribute("TANGENT");
            if (tangents != primitive.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<Vec4>(asset, asset.accessors[(*tangents).accessorIndex],
                    [&](Vec4 tangent, size_t index) {
                        vertices[initial_vertex_index + index].tangent = tangent;
                    }
                );
            }

            // UV
            auto tex_coord = primitive.findAttribute("TEXCOORD_0");
            if (tex_coord != primitive.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<Vec2>(asset, asset.accessors[(*tex_coord).accessorIndex],
                    [&](Vec2 uv, size_t index) {
                        vertices[initial_vertex_index + index].uv_x = uv.x;
                        vertices[initial_vertex_index + index].uv_y = uv.y;
                    }
                );
            }

            // Color
            auto color_attribute = primitive.findAttribute("COLOR_1");
            if (color_attribute != primitive.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<Vec4>(asset, asset.accessors[(*color_attribute).accessorIndex],
                    [&](Vec4 color, size_t index) {
                        vertices[initial_vertex_index + index].color = color;
                    }
                );
            }

            // Joints
            auto joint_attribute = primitive.findAttribute(std::format("JOINTS_{}", 0));
            if (joint_attribute != primitive.attributes.end()) {
                skinning_data.resize(vertices.size());

                fastgltf::Accessor& joint_accessor = asset.accessors[joint_attribute->accessorIndex];
                fastgltf::iterateAccessorWithIndex<Vec4>(asset, joint_accessor,
                    [&](Vec4 joint, size_t index) {
                        skinning_data[initial_vertex_index + index] = SkinningData{
                            .joint_ids = {
                                uint32_t(asset.skins[0].joints[uint32_t(joint.x)]),
                                uint32_t(asset.skins[0].joints[uint32_t(joint.y)]),
                                uint32_t(asset.skins[0].joints[uint32_t(joint.z)]),
                                uint32_t(asset.skins[0].joints[uint32_t(joint.w)]),
                            }
                        };
                    }
                );

                auto weights_attribute = primitive.findAttribute(std::format("WEIGHTS_{}", 0));
                assert(weights_attribute != primitive.attributes.end());

                fastgltf::Accessor& weight_accessor = asset.accessors[weights_attribute->accessorIndex];
                fastgltf::iterateAccessorWithIndex<Vec4>(asset, weight_accessor,
                    [&](Vec4 weights, size_t index) {
                        skinning_data[initial_vertex_index + index].weights = weights;
                    }
                );
            }

            // surface record
            file << new_surface.start_index << "\n";
            file << new_surface.count << "\n";
            if (primitive.materialIndex.has_value()) {
                file << primitive.materialIndex.value() << "\n";
            } else {
                file << -1 << "\n";
            }
        }

        if (generate_collision_shape) {
            JPH::TriangleList triangles;
            triangles.reserve(indices.size() * 3);
            for (uint triangle_id = 0; triangle_id + 2 < indices.size(); triangle_id += 3) {
                JPH::Vec3 v1(vertices[indices[triangle_id]].position.x,     vertices[indices[triangle_id]].position.y,     vertices[indices[triangle_id]].position.z);
                JPH::Vec3 v2(vertices[indices[triangle_id + 1]].position.x, vertices[indices[triangle_id + 1]].position.y, vertices[indices[triangle_id + 1]].position.z);
                JPH::Vec3 v3(vertices[indices[triangle_id + 2]].position.x, vertices[indices[triangle_id + 2]].position.y, vertices[indices[triangle_id + 2]].position.z);
                triangles.emplace_back(JPH::Triangle(v1, v2, v3, 0));
            }

            JPH::PhysicsMaterialList mats;
            mats.push_back(JPH::PhysicsMaterial::sDefault);
            auto shape = (new JPH::MeshShapeSettings(triangles, mats))->Create().Get();

            std::ofstream collision_file(std::format("{}.col", p_file_path.c_str()),
                                         std::ofstream::out | std::ofstream::trunc | std::ofstream::binary);
            JPH::StreamOutWrapper stream_out(collision_file);
            uint FOURCC_JOLT = 42;
            stream_out.Write(FOURCC_JOLT);
            shape->SaveBinaryState(stream_out);
            collision_file.flush();
            collision_file.close();
        }

        file << "END\n";
        file << vertices.size() << "\n";
        file << indices.size() << "\n";
        file << skinning_data.size() << "\n";
        file << joint_data.size() << "\n";

        if (!vertices.empty()) file.write(reinterpret_cast<char*>(vertices.data()), vertices.size() * sizeof(Vertex));
        if (!indices.empty())  file.write(reinterpret_cast<char*>(indices.data()),  indices.size() * sizeof(uint32_t));
        if (!skinning_data.empty()) file.write(reinterpret_cast<char*>(skinning_data.data()), skinning_data.size() * sizeof(SkinningData));
        if (!joint_data.empty())    file.write(reinterpret_cast<char*>(joint_data.data()),    joint_data.size() * sizeof(JointData));
    }
	// ---- Nodes (nodeIndex -> parentIndex, meshIndex, localMatrix 4x4) ----
	file << "Nodes\n";
	file << asset.nodes.size() << "\n";

	std::vector<int> parent_index(asset.nodes.size(), -1);
	for (size_t p = 0; p < asset.nodes.size(); ++p) {
    	for (auto c : asset.nodes[p].children) {
        	if (c < parent_index.size())
        	    parent_index[c] = (int)p;
    	}
	}

	for (size_t ni = 0; ni < asset.nodes.size(); ++ni) {
    	auto& n = asset.nodes[ni];

    	int mesh_index = n.meshIndex.has_value() ? (int)n.meshIndex.value() : -1;
    	file << parent_index[ni] << "\n";
    	file << mesh_index << "\n";

    	// local matrix: TRS or matrix
    glm::mat4 M(1.0f);
    if (std::holds_alternative<fastgltf::TRS>(n.transform)) {
        auto trs = std::get<fastgltf::TRS>(n.transform);
        glm::vec3 T(trs.translation[0], trs.translation[1], trs.translation[2]);
        glm::quat R(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]); // w,x,y,z
        glm::vec3 S(trs.scale[0], trs.scale[1], trs.scale[2]);

        M = glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) * glm::scale(glm::mat4(1.0f), S);
    } else if (std::holds_alternative<fastgltf::math::fmat4x4>(n.transform)) {
        auto mm = std::get<fastgltf::math::fmat4x4>(n.transform);
        // fastgltf matrix is column-major compatible with glm usually
        std::memcpy(&M[0][0], mm.data(), sizeof(float) * 16);
    } else {
        // identity
    }

    // пишем 16 float построчно (как у тебя остальной формат — текстовый)
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            file << M[c][r] << "\n"; // glm is column-major, but writing consistently; read same way
        }
    }
}
    file.close();

    std::ofstream import_settings_output_stream(import_settings_path.c_str());
    import_settings_output_stream << import_settings;
    import_settings_output_stream.close();
}