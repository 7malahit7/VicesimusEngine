#include <components/animation_player.h>
#include <components/skeleton.h>
#include <components/model_data.h>
#include <core/node.h>
#include <print>
#include <yaml.h>

void AnimationPlayer::update(double delta) {
    if (!skeleton) {
        if (!playing || library.animations.find(current_animation) == library.animations.end()) {
            updating = false;
            return;
        }

        updating = true;
        auto it = library.animations.find(current_animation);
		if (!playing || it == library.animations.end()) {
    		updating = false;
    		return;
		}
		const Animation &animation = it->second;

        current_time += float(delta);
        if (animation.length > 0.0f && current_time >= animation.length) {
            current_time = std::fmod(current_time, animation.length);
        }

        for (auto& [node_index, channel] : animation.channels) {
            auto md = node->get_component<ModelData>();
			if (!md) continue;
			if (node_index >= md->gltf_nodes.size()) continue;
			auto target = md->gltf_nodes[node_index];
			if (!target) continue;

            // rotation
            if (channel.rotation_keyframes.size() > 2) {
                uint32_t current_index = channel_rotation_index[node_index];
                if (current_index >= channel.rotation_keyframes.size()) current_index = 0;

                uint32_t next_index = (current_index + 1) < channel.rotation_keyframes.size() ?
                    (current_index + 1) : 0;

                auto current_keyframe = channel.rotation_keyframes[current_index];
                auto next_keyframe    = channel.rotation_keyframes[next_index];

                bool advance_keyframe = (current_keyframe.time < next_keyframe.time) ?
                    (current_time > next_keyframe.time || current_time < current_keyframe.time) :
                    (current_time > next_keyframe.time && current_time < current_keyframe.time);

                while (advance_keyframe) {
                    current_index = next_index;
                    next_index = (current_index + 1) < channel.rotation_keyframes.size() ?
                        (current_index + 1) : 0;

                    current_keyframe = channel.rotation_keyframes[current_index];
                    next_keyframe    = channel.rotation_keyframes[next_index];
                    channel_rotation_index[node_index] = current_index;

                    advance_keyframe = (current_keyframe.time < next_keyframe.time) ?
                        (current_time > next_keyframe.time || current_time < current_keyframe.time) :
                        (current_time > next_keyframe.time && current_time < current_keyframe.time);
                }

                float time_difference = next_keyframe.time - current_keyframe.time;
                if (time_difference < 0.0f) time_difference += animation.length;

                float elapsed_time = current_time - current_keyframe.time;
                if (elapsed_time < 0.0f) elapsed_time += animation.length;

                float weight = (time_difference > 0.0f) ? (elapsed_time / time_difference) : 0.0f;

                Quaternion blended_rotation = glm::slerp(current_keyframe.rotation, next_keyframe.rotation, weight);
                target->set_rotation(blended_rotation);
            }
            else if (channel.rotation_keyframes.size() == 2) {
                auto current_keyframe = channel.rotation_keyframes[0];
                auto next_keyframe    = channel.rotation_keyframes[1];

                float time_difference = next_keyframe.time - current_keyframe.time;
                if (time_difference < 0.0f) time_difference += animation.length;

                float elapsed_time = current_time - current_keyframe.time;
                if (elapsed_time < 0.0f) elapsed_time += animation.length;

                float weight = (time_difference > 0.0f) ? (elapsed_time / time_difference) : 0.0f;

                Quaternion blended_rotation = glm::slerp(current_keyframe.rotation, next_keyframe.rotation, weight);
                target->set_rotation(blended_rotation);
            }
            else if (channel.rotation_keyframes.size() == 1) {
                const Quaternion r = channel.rotation_keyframes[0].rotation;
                target->set_rotation(r);
            }

            if (channel.position_keyframes.size() > 2) {
                uint current_index = channel_position_index[node_index];
                if (current_index >= channel.position_keyframes.size()) current_index = 0;

                uint next_index = (current_index < (channel.position_keyframes.size() - 1)) ?
                    (current_index + 1) : 0;

                auto current_keyframe = channel.position_keyframes[current_index];
                auto next_keyframe    = channel.position_keyframes[next_index];

                bool advance_keyframe = (current_keyframe.time < next_keyframe.time) ?
                    (current_time > next_keyframe.time || current_time < current_keyframe.time) :
                    (current_time > next_keyframe.time && current_time < current_keyframe.time);

                while (advance_keyframe) {
                    current_index = next_index;
                    next_index = (current_index < (channel.position_keyframes.size() - 1)) ?
                        (current_index + 1) : 0;

                    current_keyframe = channel.position_keyframes[current_index];
                    next_keyframe    = channel.position_keyframes[next_index];
                    channel_position_index[node_index] = current_index;

                    advance_keyframe = (current_keyframe.time < next_keyframe.time) ?
                        (current_time > next_keyframe.time || current_time < current_keyframe.time) :
                        (current_time > next_keyframe.time && current_time < current_keyframe.time);
                }

                float time_difference = next_keyframe.time - current_keyframe.time;
                if (time_difference < 0.0f) time_difference += animation.length;

                float elapsed_time = current_time - current_keyframe.time;
                if (elapsed_time < 0.0f) elapsed_time += animation.length;

                float weight = (time_difference > 0.0f) ? (elapsed_time / time_difference) : 0.0f;

                Vec3 blended_position =
                    (current_keyframe.position) * (1.0f - weight) + next_keyframe.position * weight;

                target->set_position(blended_position);
            }
            else if (channel.position_keyframes.size() == 2) {
                auto current_keyframe = channel.position_keyframes[0];
                auto next_keyframe    = channel.position_keyframes[1];

                float time_difference = next_keyframe.time - current_keyframe.time;
                if (time_difference < 0.0f) time_difference += animation.length;

                float elapsed_time = current_time - current_keyframe.time;
                if (elapsed_time < 0.0f) elapsed_time += animation.length;

                float weight = (time_difference > 0.0f) ? (elapsed_time / time_difference) : 0.0f;

                Vec3 blended_position =
                    (current_keyframe.position) * (1.0f - weight) + next_keyframe.position * weight;

                target->set_position(blended_position);
            }
            else if (channel.position_keyframes.size() == 1) {
                const Vec3 p = channel.position_keyframes[0].position;
                target->set_position(p);
            }

            target->refresh_transform(Transform());
        }

        updating = false;
        return;
    }

    const bool node_mode = (skeleton == nullptr);

    updating = true;

	auto it = library.animations.find(current_animation);
	if (!playing || it == library.animations.end()) {
	    updating = false;
    	return;
	}
	const Animation &animation = it->second;

    current_time = current_time + float(delta);
    bool looped = current_time >= animation.length;

    if (looped) {
        finished.emit(current_animation);
        if (interrupted) {
            interrupted = false;
            updating = false;
            return;
        }
        if (animation.length > 0.0f) {
            current_time -= animation.length;
        } else {
            current_time = 0.0f;
        }
    }

    const bool allow_root_motion = (!node_mode && skeleton);

    for (auto [bone_index, channel] : animation.channels) {
        if (channel.position_keyframes.size() > 2) {
            uint current_index = channel_position_index[bone_index];
            if (current_index >= channel.position_keyframes.size()) current_index = 0;

            uint next_index = (current_index < (channel.position_keyframes.size() - 1)) ?
                (current_index + 1) : 0;

            auto current_keyframe = channel.position_keyframes[current_index];
            auto next_keyframe    = channel.position_keyframes[next_index];

            bool advance_keyframe = (current_keyframe.time < next_keyframe.time) ?
                (current_time > next_keyframe.time || current_time < current_keyframe.time) :
                (current_time > next_keyframe.time && current_time < current_keyframe.time);

            while (advance_keyframe) {
                current_index = next_index;
                next_index = (current_index < (channel.position_keyframes.size() - 1)) ?
                    (current_index + 1) : 0;

                current_keyframe = channel.position_keyframes[current_index];
                next_keyframe    = channel.position_keyframes[next_index];
                channel_position_index[bone_index] = current_index;

                advance_keyframe = (current_keyframe.time < next_keyframe.time) ?
                    (current_time > next_keyframe.time || current_time < current_keyframe.time) :
                    (current_time > next_keyframe.time && current_time < current_keyframe.time);
            }

            float time_difference = next_keyframe.time - current_keyframe.time;
            if (time_difference < 0.0f) time_difference += animation.length;

            float elapsed_time = current_time - current_keyframe.time;
            if (elapsed_time < 0.0f) elapsed_time += animation.length;

            float weight = (time_difference > 0.0f) ? (elapsed_time / time_difference) : 0.0f;

            Vec3 blended_position =
                (current_keyframe.position) * (1.0f - weight) + next_keyframe.position * weight;

            if (allow_root_motion && (bone_index == (uint32_t)skeleton->root_motion_index)) {
                if (looped) {
                    previous_root_motion_position -=
                        channel.position_keyframes[channel.position_keyframes.size() - 1].position;
                }
                root_motion_velocity = blended_position - previous_root_motion_position;
                previous_root_motion_position = blended_position;
            } else {
                if (!node_mode) skeleton->set_bone_position(bone_index, blended_position);
                else            node->set_position(blended_position);
            }
        }
        else if (channel.position_keyframes.size() == 2) {
            auto current_keyframe = channel.position_keyframes[0];
            auto next_keyframe    = channel.position_keyframes[1];

            float time_difference = next_keyframe.time - current_keyframe.time;
            if (time_difference < 0.0f) time_difference += animation.length;

            float elapsed_time = current_time - current_keyframe.time;
            if (elapsed_time < 0.0f) elapsed_time += animation.length;

            float weight = (time_difference > 0.0f) ? (elapsed_time / time_difference) : 0.0f;

            Vec3 blended_position =
                (current_keyframe.position) * (1.0f - weight) + next_keyframe.position * weight;

            if (allow_root_motion && (bone_index == (uint32_t)skeleton->root_motion_index)) {
                if (looped) {
                    previous_root_motion_position -=
                        channel.position_keyframes[channel.position_keyframes.size() - 1].position;
                }
                root_motion_velocity = blended_position - previous_root_motion_position;
                previous_root_motion_position = blended_position;
            } else {
                if (!node_mode) skeleton->set_bone_position(bone_index, blended_position);
                else            node->set_position(blended_position);
            }
        }
        else if (channel.position_keyframes.size() == 1) {
            const Vec3 p = channel.position_keyframes[0].position;
            if (!node_mode) skeleton->set_bone_position(bone_index, p);
            else            node->set_position(p);
        }

        if (channel.rotation_keyframes.size() > 2) {
            uint32_t current_index = channel_rotation_index[bone_index];
            if (current_index >= channel.rotation_keyframes.size()) current_index = 0;

            uint32_t next_index = (current_index + 1) < channel.rotation_keyframes.size() ?
                (current_index + 1) : 0;

            auto current_keyframe = channel.rotation_keyframes[current_index];
            auto next_keyframe    = channel.rotation_keyframes[next_index];

            bool advance_keyframe = (current_keyframe.time < next_keyframe.time) ?
                (current_time > next_keyframe.time || current_time < current_keyframe.time) :
                (current_time > next_keyframe.time && current_time < current_keyframe.time);

            while (advance_keyframe) {
                current_index = next_index;
                next_index = (current_index + 1) < channel.rotation_keyframes.size() ?
                    (current_index + 1) : 0;

                current_keyframe = channel.rotation_keyframes[current_index];
                next_keyframe    = channel.rotation_keyframes[next_index];
                channel_rotation_index[bone_index] = current_index;

                advance_keyframe = (current_keyframe.time < next_keyframe.time) ?
                    (current_time > next_keyframe.time || current_time < current_keyframe.time) :
                    (current_time > next_keyframe.time && current_time < current_keyframe.time);
            }

            float time_difference = next_keyframe.time - current_keyframe.time;
            if (time_difference < 0.0f) time_difference += animation.length;

            float elapsed_time = current_time - current_keyframe.time;
            if (elapsed_time < 0.0f) elapsed_time += animation.length;

            float weight = (time_difference > 0.0f) ? (elapsed_time / time_difference) : 0.0f;

            Quaternion blended_rotation = glm::slerp(current_keyframe.rotation, next_keyframe.rotation, weight);

            if (!node_mode) skeleton->set_bone_rotation(bone_index, blended_rotation);
            else            node->set_rotation(blended_rotation);
        }
        else if (channel.rotation_keyframes.size() == 2) {
            auto current_keyframe = channel.rotation_keyframes[0];
            auto next_keyframe    = channel.rotation_keyframes[1];

            float time_difference = next_keyframe.time - current_keyframe.time;
            if (time_difference < 0.0f) time_difference += animation.length;

            float elapsed_time = current_time - current_keyframe.time;
            if (elapsed_time < 0.0f) elapsed_time += animation.length;

            float weight = (time_difference > 0.0f) ? (elapsed_time / time_difference) : 0.0f;

            Quaternion blended_rotation = glm::slerp(current_keyframe.rotation, next_keyframe.rotation, weight);

            if (!node_mode) skeleton->set_bone_rotation(bone_index, blended_rotation);
            else            node->set_rotation(blended_rotation);
        }
        else if (channel.rotation_keyframes.size() == 1) {
            const Quaternion r = channel.rotation_keyframes[0].rotation;
            if (!node_mode) skeleton->set_bone_rotation(bone_index, r);
            else            node->set_rotation(r);
        }
    }

    if (!node_mode && skeleton) {
        skeleton->node->refresh_transform(Transform());
    } else {
        node->refresh_transform(Transform());
    }

    updating = false;
}

void AnimationPlayer::play(std::string p_animation_name) {
    channel_position_index.clear();
    channel_rotation_index.clear();
    previous_root_motion_position = Vec3(0.0f);

    if (updating) {
        interrupted = true;
    }

    current_animation = p_animation_name;
    if (library.animations.find(current_animation) == library.animations.end()) {
        playing = false;
        return;
    }
	auto it = library.animations.find(current_animation);
	if (it == library.animations.end()) {
    	playing = false;
    	return;
	}
	const Animation &animation = it->second;

	for (const auto &kv : animation.channels) {
    	channel_position_index[kv.first] = 0;
    	channel_rotation_index[kv.first] = 0;
	}

    current_time = 0.0f;
    playing = true;
}

void AnimationPlayer::initialize() {
    auto md = node->get_component<ModelData>();
	if (!md) {
        print("AnimationPlayer: no ModelData on node");
        playing = false;
        return;
    }
    skeleton = md->skeleton;
    library  = md->animation_library;

    if (!skeleton) {
        print("AnimationPlayer: no Skeleton; using node animation mode");
    }
}

COMPONENT_FACTORY_IMPL(AnimationPlayer, animation_player) {

}