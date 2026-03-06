#pragma once

#include <vector>
#include <memory>
#include <core/node.h>

class BoneContainer
{
public:

    std::vector<Ref<Node>> bones;

    Ref<Node> get(int id)
    {
        if (id < 0 || id >= bones.size())
            return nullptr;

        return bones[id];
    }

    size_t size() const
    {
        return bones.size();
    }

    void load_from_model(const std::shared_ptr<ModelData>& model)
    {
        bones.clear();

        if (!model)
            return;

        for (auto& node : model->gltf_nodes)
        {
            bones.push_back(node);
        }
    }
};