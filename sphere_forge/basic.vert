/*
 * Copyright (c) 2018-2020 The Forge Interactive Inc.
 * 
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
*/

// Shader for simple shading with a point light
// for planets in Unit Test 12 - Transformations


cbuffer uniformBlock : register(b0, UPDATE_FREQ_PER_FRAME)
{
	float4x4 mvp;
    float4x4 world;
    float4 color;

    // Point Light Information
    float3 lightPosition;
    float3 lightColor;
};

struct VSInput
{
    float4 Position : POSITION;
    float4 Normal : NORMAL;
};

struct VSOutput {
	float4 Position : SV_POSITION;
    float4 Color : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput result;
    float4x4 tempMat = mul(mvp, world);
    result.Position = mul(tempMat, input.Position);

    float4 normal = normalize(mul(world, float4(input.Normal.xyz, 0.0f))); // Assume uniform scaling
    float4 pos = mul(world, float4(input.Position.xyz, 1.0f));

    float lightIntensity = 1.0f;
    float quadraticCoeff = 1.2;
    float ambientCoeff = 0.4;

    float3 lightDir = normalize(lightPosition - pos.xyz);

    float distance = length(lightDir);
    float attenuation = 1.0 / (quadraticCoeff * distance * distance);
    float intensity = lightIntensity * attenuation;

    float3 baseColor = color.xyz;
    float3 blendedColor = mul(lightColor * baseColor, lightIntensity);
    float3 diffuse = mul(blendedColor, max(dot(normal.xyz, lightDir), 0.0));
    float3 ambient = mul(baseColor, ambientCoeff);
    result.Color = float4(diffuse + ambient, 1.0);

    return result;
}
