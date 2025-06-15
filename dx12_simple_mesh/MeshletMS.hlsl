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
                  SRV(t0), \
                  SRV(t1), \
                  SRV(t2), \
                  SRV(t3)"

struct Constants
{
    float4x4 World;
    float4x4 WorldView;
    float4x4 WorldViewProj;
    uint     DrawMeshlets;
};

struct Vertex
{
    float3 Position;
    float3 Normal;
};

struct VertexOut
{
    float4 PositionHS   : SV_Position;
    float3 PositionVS   : POSITION0;
};

ConstantBuffer<Constants> Globals             : register(b0);
//ConstantBuffer<MeshInfo>  MeshInfo            : register(b1);

StructuredBuffer<Vertex>  Vertices            : register(t0);
//StructuredBuffer<Meshlet> Meshlets            : register(t1);
//ByteAddressBuffer         UniqueVertexIndices : register(t2);
//StructuredBuffer<uint>    PrimitiveIndices    : register(t3);


[RootSignature(ROOT_SIG)]
[NumThreads(128, 1, 1)]
[OutputTopology("triangle")]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid : SV_GroupID,
    out indices uint3 tris[126],
    out vertices VertexOut verts[64]
)
{
    //Meshlet m = Meshlets[MeshInfo.MeshletOffset + gid];

    SetMeshOutputCounts(gid, gid);

    //if (gtid < m.PrimCount)
    //{
    ////    tris[gtid] = GetPrimitive(m, gtid);
    ////}//

    //if (gtid < m.VertCount)
    tris[gtid] = uint3(gid, gid + 1, gid + 2);

    verts[gtid].PositionVS = float4(Vertices[gtid].Position, 1);
    verts[gtid].PositionHS = mul(float4(Vertices[gtid].Position, 1), Globals.WorldViewProj);
}