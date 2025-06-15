//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************


#define ROOT_SIG "CBV(b0), \
                  RootConstants(b1, num32bitconstants=2), \
                  SRV(t0),\
                  UAV(u0),"

struct Constants
{
    float4x4 World;
    float4x4 WorldView;
    float4x4 WorldViewProj;
    uint     DrawMeshlets;
};

struct Vertex
{
    float4 Position;
};

struct VertexOut
{
    float4 Position   : SV_Position;
};

ConstantBuffer<Constants> Globals             : register(b0);
StructuredBuffer<Vertex>  Vertices            : register(t0);
RWStructuredBuffer<float4> debugOutput        : register(u0);


[RootSignature(ROOT_SIG)]
[NumThreads(128, 1, 1)]
[OutputTopology("triangle")]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    //out indices uint3 tris[126],
    out vertices VertexOut verts[64]
)
{

    SetMeshOutputCounts(9, 3);

    float4 output_pos = mul(Vertices[gtid].Position, Globals.WorldViewProj);
    
    if (gtid == 0)
      debugOutput[0] = float4(1.0, 2.0, 3.0, 4.0);
    
    verts[gtid].Position = output_pos;
    
}