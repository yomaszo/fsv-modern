// SPDX-License-Identifier: MIT

#version 140

in vec3 fragPos;
in vec3 fragNormal;
in vec4 lightPos;
in vec3 fragVColor;
in float fragHighlight;

out vec4 outputColor;

uniform vec4 color;
uniform float ambient;
uniform float diffuse;
uniform float specular;
uniform bool lightning_enabled;
uniform bool use_vertex_color;
uniform bool selection_mode;

void main() {
  if (selection_mode) {
    outputColor = vec4(fragVColor, 1.0);
    return;
  }
  vec4 base_color = use_vertex_color ? vec4(fragVColor, 1.0) : color;

  if (!lightning_enabled) {
    outputColor = base_color;
    return;
  }

  vec3 light_color = vec3(1.0, 1.0, 1.0);
  // Ambient light
  vec3 ambient_light = ambient * light_color;

  // Diffuse light
  vec3 lightDir;
  if (lightPos.w == 0.0)  // Light at infinity
    lightDir = normalize(lightPos.xyz);
  else
    lightDir = normalize(lightPos.xyz - fragPos);
  vec3 fragNN = normalize(fragNormal);
  float diffuse_refl = max(dot(fragNN, lightDir), 0.0);
  vec3 diffuse_light = diffuse_refl * diffuse * light_color;

  // Specular light
  vec3 viewPos = vec3(0.0, 0.0, 0.0);
  vec3 viewDir = normalize(viewPos - fragPos);
  vec3 reflectDir = reflect(-lightDir, fragNN);
  float spec = pow(max(dot(viewDir, reflectDir), 0.0), 2);
  vec3 spec_light = specular * spec * light_color;
  // Narrow clear-coat reflection on top of the broad legacy highlight gives
  // pastel blocks a polished, glass-like surface without alpha blending.
  float clear_coat = pow(max(dot(viewDir, reflectDir), 0.0), 28.0);

  // Final color from lightning calculation
  outputColor = vec4(((ambient_light + diffuse_light + spec_light) * base_color.rgb) +
                     vec3(0.58, 0.82, 0.92) * clear_coat * 0.22, base_color.a);

  // A restrained Fresnel rim gives the opaque pastel surfaces a glass-like
  // edge highlight. Selection mode returns above, so pick colors stay exact.
  float rim = pow(1.0 - abs(dot(fragNN, viewDir)), 3.0);
  float rim_light = min(rim * (0.24 + 0.34 * fragHighlight), 0.58);
  outputColor.rgb = mix(outputColor.rgb, vec3(0.86, 0.98, 1.0), rim_light);


  // For debugging, uncomment this. Also set fragNormal to flat in both vertex
  // and fragment shader out/in
  //outputColor = 0.9999 * vec4(abs(fragNN), 1.0) + 0.0001 * outputColor;
}
