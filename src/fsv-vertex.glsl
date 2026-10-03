// SPDX-License-Identifier: MIT

#version 140

in vec3 position;
in vec3 normal;
in vec3 vcolor;
in float node_id;
in vec4 instance_bounds;
in vec4 instance_shape;
in vec4 instance_transform_color;
in float instance_node_id;

out vec3 fragPos;
out vec3 fragNormal;
out vec4 lightPos;
out vec3 fragVColor;
out float fragHighlight;

uniform mat4 mvp;
uniform mat4 modelview;
uniform mat3 normal_matrix;
uniform vec4 light_pos;
uniform bool lightning_enabled;
uniform bool use_vertex_color;
uniform bool use_node_id;
uniform bool selection_mode;
uniform float highlighted_node_id;
uniform bool instanced_geometry;
uniform bool instanced_lod_dynamic;


void main() {
  vec4 pos;
  vec3 vertex_normal = normal;
  vec3 vertex_color = vcolor;
  float vertex_node_id = use_node_id ? node_id : 0.0;

  if (instanced_geometry) {
    float u = position.x;
    float v = position.y;
    float top = position.z;
    float height = instance_shape.x;
    float offset_x = instance_shape.y;
    float offset_y = instance_shape.z;
    // A negative scale marks a sub-pixel-ish node: keep its top face, but
    // collapse the side faces to degenerate triangles to avoid spending
    // fragment work on tiny beveled boxes.
    float z_scale = abs(instance_shape.w);
    bool flat_lod = instance_shape.w < 0.0;
    if (instanced_lod_dynamic) {
      vec4 center = vec4(0.5 * (instance_bounds.x + instance_bounds.z),
                         0.5 * (instance_bounds.y + instance_bounds.w),
                         instance_transform_color.x + 0.5 * z_scale * height,
                         1.0);
      float clip_w = (mvp * center).w;
      if (clip_w > 0.0001) {
        float node_width = instance_bounds.z - instance_bounds.x;
        float node_depth = instance_bounds.w - instance_bounds.y;
        float ndc_w = (abs(mvp[0][0]) * node_width +
                       abs(mvp[1][0]) * node_depth +
                       abs(mvp[2][0]) * z_scale * height) / clip_w;
        float ndc_h = (abs(mvp[0][1]) * node_width +
                       abs(mvp[1][1]) * node_depth +
                       abs(mvp[2][1]) * z_scale * height) / clip_w;
        flat_lod = max(ndc_w, ndc_h) < 0.018;
      }
    }
    if (flat_lod && abs(normal.z) < 0.5)
      top = 1.0;

    pos = vec4(mix(instance_bounds.x, instance_bounds.z, u) +
                 top * (1.0 - 2.0 * u) * offset_x,
               mix(instance_bounds.y, instance_bounds.w, v) +
                 top * (1.0 - 2.0 * v) * offset_y,
               instance_transform_color.x + z_scale * top * height,
               1.0);

    if (abs(normal.x) > 0.5) {
      float side_length = length(vec2(offset_x, height));
      vertex_normal = vec3(sign(normal.x) * height / side_length, 0.0,
                           offset_x / (side_length * z_scale));
    } else if (abs(normal.y) > 0.5) {
      float side_length = length(vec2(offset_y, height));
      vertex_normal = vec3(0.0, sign(normal.y) * height / side_length,
                           offset_y / (side_length * z_scale));
    } else {
      vertex_normal = vec3(0.0, 0.0, 1.0 / z_scale);
    }
    vertex_color = instance_transform_color.yzw;
    vertex_node_id = use_node_id ? instance_node_id : 0.0;
  } else {
    pos = vec4(position, 1.0);
  }

  gl_Position = mvp * pos;

  fragHighlight = vertex_node_id > 0.0 &&
                  abs(vertex_node_id - highlighted_node_id) < 0.5 ? 1.0 : 0.0;

  if (use_vertex_color) {
    if (selection_mode) {
      float id = floor(vertex_node_id + 0.5);
      fragVColor = vec3(mod(id, 256.0), mod(floor(id / 256.0), 256.0), floor(id / 65536.0)) / 255.0;
    } else {
      if (vertex_node_id > 0.0 && abs(vertex_node_id - highlighted_node_id) < 0.5)
        vertex_color *= 1.3;
      fragVColor = vertex_color;
    }
  }

  if (lightning_enabled) {
    lightPos = modelview * light_pos;

    // Position of vertex in camera coordinates interpolated to frag coords
    vec4 fragTmp = modelview * pos;
    fragPos = fragTmp.xyz / fragTmp.w;

    fragNormal = normal_matrix * vertex_normal;
  }
}
