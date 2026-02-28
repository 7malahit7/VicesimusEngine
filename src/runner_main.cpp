#include <prosper.h>
#include <core/node.h>
#include <core/scene_graph.h>

#include <components/model_data.h>
#include <components/animation_player.h>

#include <cstring>
#include <string>

extern SceneGraph scene;

static const char* get_arg(int argc, char** argv, const char* key) {
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    }
    return nullptr;
}

int main(int argc, char** argv) {
    if (!prosper::initialize()) return 1;
    print("RUNNER: init ok");

    const char* model_path = get_arg(argc, argv, "--model");
    if (!model_path) {
        print("RUNNER: usage: ./prosper_runner --model <path-to-gltf-or-glb>");
        while (prosper::iterate()) {}
        prosper::quit();
        return 0;
    }

    auto model_node = Node::create("Model");
    scene.root->add_child(model_node);
    model_node->refresh_transform(Transform());

    auto md = model_node->add_component<ModelData>();
    md->model_path = model_path;
    md->skinned = false;
    md->initialize();

    auto ap = model_node->add_component<AnimationPlayer>();
    ap->initialize();
    print("RUNNER: loading model '%s'", model_path);
    print("RUNNER: entering loop");

    bool started = false;
    while (prosper::iterate()) {
        if (!started && md && ap) {
            if (md->animation_library.animations.empty()) continue;

            std::string anim_name = "anim_16";
            if (md->animation_library.animations.find(anim_name) == md->animation_library.animations.end()) {
                anim_name = md->animation_library.animations.begin()->first;
            }

            ap->play(anim_name);
            print("RUNNER: autoplay animation '%s'", anim_name.c_str());
            started = true;
        }
    }

    prosper::quit();
    return 0;
}