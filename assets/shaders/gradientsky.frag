/**************************************************************************
This file is part of JahshakaVR, VR Authoring Toolkit
http://www.jahshaka.com
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#version 150 core

uniform vec4 color_top;
uniform vec4 color_mid;
uniform vec4 color_bot;
uniform float middle_offset;

in vec3 v_texCoord;

out vec4 fragColor;

// https://answers.unity.com/questions/1108472/3-color-linear-gradient-shader.html
// https://johnflux.com/2016/03/16/four-point-gradient-as-a-shader/ (useful maybe in the future)
vec4 gradient(float offset) {
	vec4 grad = mix(color_bot, color_mid, offset / middle_offset) * step(offset, middle_offset);
	grad += mix(color_mid, color_top, (offset - middle_offset) / (1.0 - middle_offset)) * (1.0 - step(offset, middle_offset));
	grad.a = 1.0;	
	return grad;
}

void main()
{
    fragColor = gradient(normalize(v_texCoord).y * 0.5 + 0.5);
}