#include <components/model_data.h>

#include <core/resource_manager.h>
#include <core/node.h>

#include <components/bone.h>
#include <components/mesh_instance.h>
#include <components/skinned_mesh.h>
#include <components/skeleton.h>
#include <components/physics/static_body.h>

#include <rendering/renderer.h>
#include <resources/animation.h>

#include <yaml.h>

#include <Jolt/Jolt.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

extern Renderer gRenderer;


void ModelData::cleanup() {
    Device device = renderer->device;

    descriptor_pool.destroy_pools(device);
    renderer->destroy_buffer(material_data_buffer);

    for (auto& [key, value] : meshes)
        value->unreference();

    for (auto& texture : textures)
        (*texture).unreference();

    for (auto& sampler : samplers)
        device.destroySampler(sampler);
}

static bool read_expected_line(std::fstream& file,
                               std::string& line,
                               const char* expected) {
    std::getline(file, line);
    if (!file.good()) return false;
    if (line != expected) return false;
    return true;
}

void ModelData::initialize() {

    if (model_path.empty())
        return;

    std::string imported_path =
        std::format("{}.imported", model_path);

    if (!std::filesystem::exists(imported_path))
        import_gltf_scene(&gRenderer, model_path);

    if (!std::filesystem::exists(imported_path))
        return;

    std::fstream file(imported_path,
                      std::fstream::in | std::fstream::binary);

    if (!file.is_open())
        return;

    materials["default"] =
        std::make_shared<MaterialInstance>(
            gRenderer.default_material);

    std::string line;


    if (!read_expected_line(file, line, "Samplers")) return;
    std::getline(file, line);
    uint32_t sampler_count = std::stoi(line);

    std::vector<SamplerCreateInfo> sampler_infos(sampler_count);

    if (sampler_count > 0) {
        file.read(reinterpret_cast<char*>(sampler_infos.data()),
                  sampler_count * sizeof(SamplerCreateInfo));

        for (auto& s : sampler_infos) {
            auto [res, sampler] =
                gRenderer.device.createSampler(s);
            (void)res;
            samplers.push_back(sampler);
        }
    }

    if (!read_expected_line(file, line, "Textures")) return;
    std::getline(file, line);
    uint32_t texture_count = std::stoi(line);

    std::vector<std::string> texture_paths(texture_count);
    for (uint32_t i = 0; i < texture_count; ++i)
        std::getline(file, texture_paths[i]);

    if (!read_expected_line(file, line, "Materials")) return;
    std::getline(file, line);
    uint32_t material_count = std::stoi(line);

    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
        { DescriptorType::eCombinedImageSampler, 3 },
        { DescriptorType::eUniformBuffer, 3 },
        { DescriptorType::eStorageBuffer, 1 }
    };

    descriptor_pool.init_pool(
        gRenderer.device,
        material_count,
        sizes);

    material_data_buffer =
        gRenderer.create_buffer(
            sizeof(MaterialMetallicRoughness::MaterialConstants)
                * material_count,
            BufferUsageFlagBits::eUniformBuffer,
            VMA_MEMORY_USAGE_CPU_TO_GPU);

    auto* scene_constants =
        (MaterialMetallicRoughness::MaterialConstants*)
        material_data_buffer.info.pMappedData;

    for (uint32_t i = 0; i < material_count; ++i) {

        MaterialMetallicRoughness::MaterialConstants constants{};
        constants.albedo_factors = Vec4(1.0f);
        constants.metal_roughness_factors =
            Vec4(0.0f, 1.0f, 0.0f, 0.0f);

        std::getline(file, line); constants.albedo_factors.r = std::stof(line);
        std::getline(file, line); constants.albedo_factors.g = std::stof(line);
        std::getline(file, line); constants.albedo_factors.b = std::stof(line);
        std::getline(file, line); constants.albedo_factors.a = std::stof(line);
        std::getline(file, line); constants.metal_roughness_factors.r = std::stof(line);
        std::getline(file, line); constants.metal_roughness_factors.g = std::stof(line);

        scene_constants[i] = constants;

        for (int skip = 0; skip < 6; ++skip)
            std::getline(file, line);
    }

    if (!read_expected_line(file, line, "Animations")) return;
    std::getline(file, line);
    uint32_t animation_count = std::stoi(line);

    for (uint32_t i = 0; i < animation_count; ++i) {

        Animation animation{};
        std::getline(file, animation.name);

        std::getline(file, line);
        animation.length = std::stof(line);

        std::getline(file, line);
        uint32_t channel_count = std::stoi(line);

        for (uint32_t c = 0; c < channel_count; ++c) {

            std::getline(file, line);
            uint32_t node_index = std::stoi(line);

            animation.channels[node_index] =
                AnimationChannel{};
            auto& channel =
                animation.channels[node_index];

            std::getline(file, line);
            uint32_t pos_count = std::stoi(line);

            for (uint32_t p = 0; p < pos_count; ++p) {
                KeyframePosition k{};
                std::getline(file, line); k.time = std::stof(line);
                std::getline(file, line); k.position.x = std::stof(line);
                std::getline(file, line); k.position.y = std::stof(line);
                std::getline(file, line); k.position.z = std::stof(line);
                channel.position_keyframes.push_back(k);
            }

            std::getline(file, line);
            uint32_t rot_count = std::stoi(line);

            for (uint32_t r = 0; r < rot_count; ++r) {
                KeyframeRotation k{};
                std::getline(file, line); k.time = std::stof(line);
                std::getline(file, line); k.rotation.x = std::stof(line);
                std::getline(file, line); k.rotation.y = std::stof(line);
                std::getline(file, line); k.rotation.z = std::stof(line);
                std::getline(file, line); k.rotation.w = std::stof(line);
                channel.rotation_keyframes.push_back(k);
            }
        }

        animation_library.animations[animation.name] =
            animation;
    }

if (!read_expected_line(file, line, "Meshes")) return;
std::getline(file, line);
uint32_t mesh_count = std::stoi(line);

for (uint32_t mi = 0; mi < mesh_count; ++mi) {

    read_expected_line(file, line, "Mesh");
    std::getline(file, line); // mesh name (unused)

    auto mesh_id  = std::format("mesh_{}", mi);
    auto mesh_guid =
        std::format("{}::/meshes/{}",
                    imported_path,
                    mesh_id);

    auto mesh_ref =
        ResourceManager::get<Mesh>(
            mesh_guid.c_str());

    meshes[mesh_id] = mesh_ref;
    auto& mesh_res = *mesh_ref;
    mesh_res->surfaces.clear();
    mesh_res->vertex_count = 0;

    while (true) {
        std::getline(file, line);
        if (line == "END") break;

        uint32_t start_index =
            (uint32_t)std::stoi(line);

        std::getline(file, line);
        uint32_t count =
            (uint32_t)std::stoi(line);

        std::getline(file, line);
        int material_index =
            std::stoi(line);

        std::shared_ptr<MaterialInstance> mat =
            materials["default"];

        if (material_index >= 0) {
            auto it =
                materials.find(
                    std::to_string(material_index));
            if (it != materials.end())
                mat = it->second;
        }

        mesh_res->surfaces.emplace_back(
            MeshSurface{
                start_index,
                count,
                mat
            });
    }

    std::getline(file, line);
    uint32_t vertex_count =
        std::stoi(line);

    std::getline(file, line);
    uint32_t index_count =
        std::stoi(line);

    std::getline(file, line);
    uint32_t skin_count =
        std::stoi(line);

    std::getline(file, line);
    uint32_t joint_count =
        std::stoi(line);

    std::vector<Vertex> vertices(vertex_count);
    std::vector<uint32_t> indices(index_count);
    std::vector<SkinningData> skinning_data(skin_count);
    std::vector<JointData> joint_data(joint_count);

    file.read(
        reinterpret_cast<char*>(vertices.data()),
        vertex_count * sizeof(Vertex));

    file.read(
        reinterpret_cast<char*>(indices.data()),
        index_count * sizeof(uint32_t));

    if (skin_count)
        file.read(
            reinterpret_cast<char*>(skinning_data.data()),
            skin_count * sizeof(SkinningData));

    if (joint_count)
        file.read(
            reinterpret_cast<char*>(joint_data.data()),
            joint_count * sizeof(JointData));

    mesh_res->mesh_buffers =
        gRenderer.upload_mesh(indices, vertices);

    mesh_res->vertex_count =
        (uint32_t)vertices.size();

    mesh_res.set_load_status(
        LoadStatus::LOADED);

    mesh_res.reference();
}

    read_expected_line(file, line, "Nodes");
    std::getline(file, line);
    uint32_t node_count = std::stoi(line);

    struct NodeInfo {
        int parent;
        int mesh;
        glm::mat4 local;
    };

    std::vector<NodeInfo> infos(node_count);

    for (uint32_t n = 0; n < node_count; ++n) {

        std::getline(file, line);
        infos[n].parent = std::stoi(line);

        std::getline(file, line);
        infos[n].mesh = std::stoi(line);

        glm::mat4 M(1.0f);

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                std::getline(file, line);
                M[c][r] = std::stof(line);
            }

        infos[n].local = M;
    }

    std::vector<std::shared_ptr<Node>> nodes(node_count);

    for (uint32_t n = 0; n < node_count; ++n)
        nodes[n] =
            Node::create(
                std::format("gltf_node_{}",
                            n).c_str());

    gltf_nodes = nodes;

    for (uint32_t n = 0; n < node_count; ++n) {
        if (infos[n].parent >= 0)
            nodes[infos[n].parent]->add_child(
                nodes[n]);
        else
            node->add_child(nodes[n]);
    }

    for (uint32_t n = 0; n < node_count; ++n) {

        glm::mat4 M = infos[n].local;

        glm::vec3 T = glm::vec3(M[3]);

        glm::vec3 S(
            glm::length(glm::vec3(M[0])),
            glm::length(glm::vec3(M[1])),
            glm::length(glm::vec3(M[2])));

        glm::mat3 Rm;
        Rm[0] = glm::vec3(M[0]) / (S.x != 0 ? S.x : 1.0f);
        Rm[1] = glm::vec3(M[1]) / (S.y != 0 ? S.y : 1.0f);
        Rm[2] = glm::vec3(M[2]) / (S.z != 0 ? S.z : 1.0f);
        glm::quat R = glm::quat_cast(Rm);

        nodes[n]->set_position(Vec3(T.x, T.y, T.z));
        nodes[n]->set_rotation(Quaternion(R.w, R.x, R.y, R.z));

        float uniformS = (S.x + S.y + S.z) / 3.0f;
        nodes[n]->set_scale(uniformS);

        nodes[n]->refresh_transform(Transform());

        if (infos[n].mesh >= 0) {
            auto mesh_id =
                std::format("mesh_{}",
                            infos[n].mesh);
            auto it =
                meshes.find(mesh_id);

            if (it != meshes.end()) {
                auto mi =
                    nodes[n]->add_component<
                        MeshInstance>();
                mi->mesh = it->second;
            }
        }
    }

    node->refresh_transform(
        Transform());
}


COMPONENT_FACTORY_IMPL(ModelData, model) {
    renderer = &gRenderer;
    model_path = p_data["path"].as<std::string>();

    if (p_data["skinned"])
        skinned =
            p_data["skinned"].as<bool>();

    if (p_data["root_motion_index"])
        root_motion_index =
            p_data["root_motion_index"].as<int>();
}