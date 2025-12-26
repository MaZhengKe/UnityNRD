
RaytracingAccelerationStructure gWorldTlas;
StructuredBuffer<InstanceData> gIn_InstanceData ;
StructuredBuffer<PrimitiveData> gIn_PrimitiveData ;

Texture2D gIn_Textures[] : register(t31);


#if( USE_STOCHASTIC_SAMPLING == 1 )
#define TEX_SAMPLER gNearestMipmapNearestSampler
#else
#define TEX_SAMPLER gLinearMipmapLinearSampler
#endif

#if( USE_LOAD == 1 )
#define SAMPLE( coords ) Load( int3( coords ) )
#else
#define SAMPLE( coords ) SampleLevel( TEX_SAMPLER, coords.xy, coords.z )
#endif


struct GeometryProps
{
    float3 X;
    float3 Xprev;
    float3 V;
    float4 T;
    float3 N;
    float2 uv;
    float mip;
    float hitT;
    float curvature;
    uint textureOffsetAndFlags;
    uint instanceIndex;

    float3 GetXoffset( float3 offsetDir, float amount = PT_BOUNCE_RAY_OFFSET )
    {
        float viewZ = Geometry::AffineTransform( gWorldToView, X ).z;
        amount *= gUnproject * lerp( abs( viewZ ), 1.0, abs( gOrthoMode ) );

        return X + offsetDir * max( amount, 0.00001 );
    }

    bool Has( uint flag )
    { return ( textureOffsetAndFlags & ( flag << FLAG_FIRST_BIT ) ) != 0; }

    uint GetBaseTexture( )
    { return textureOffsetAndFlags & NON_FLAG_MASK; }

    float3 GetForcedEmissionColor( )
    { return ( ( textureOffsetAndFlags >> 2 ) & 0x1 ) ? float3( 1.0, 0.0, 0.0 ) : float3( 0.0, 1.0, 0.0 ); }

    bool IsMiss( )
    { return hitT == INF; }
};



struct MaterialProps
{
    float3 Lemi;
    float3 N;
    float3 T;
    float3 baseColor;
    float roughness;
    float metalness;
    float curvature;
};


float2 GetConeAngleFromAngularRadius( float mip, float tanConeAngle )
{
    // In any case, we are limited by the output resolution
    tanConeAngle = max( tanConeAngle, gTanPixelAngularRadius );

    return float2( mip, tanConeAngle );
}

float2 GetConeAngleFromRoughness( float mip, float roughness )
{
    float tanConeAngle = roughness * roughness * 0.05; // TODO: tweaked to be accurate and give perf boost

    return GetConeAngleFromAngularRadius( mip, tanConeAngle );
}


float3 GetSamplingCoords( uint textureIndex, float2 uv, float mip, int mode )
{
    float2 texSize;
    gIn_Textures[ NonUniformResourceIndex( textureIndex ) ].GetDimensions( texSize.x, texSize.y ); // TODO: if I only had it as a constant...

    // Recalculate for the current texture
    float mipNum = log2( max( texSize.x, texSize.y ) );
    mip += mipNum - MAX_MIP_LEVEL;
    if( mode == MIP_VISIBILITY )
    {
        // We must avoid using lower mips because it can lead to significant increase in AHS invocations. Mips lower than 128x128 are skipped!
        mip = min( mip, mipNum - 7.0 );
    }
    else
        mip += gMipBias * ( mode == MIP_LESS_SHARP ? 0.5 : 1.0 );
    mip = clamp( mip, 0.0, mipNum - 1.0 );

    #if( USE_STOCHASTIC_SAMPLING == 1 )
    mip = floor( mip ) + step( Rng::Hash::GetFloat( ), frac( mip ) );
    #elif( USE_LOAD == 1 )
    mip = round( mip );
    #endif

    texSize *= exp2( -mip );

    // Uv coordinates
    #if( USE_STOCHASTIC_SAMPLING == 1 )
    uv = STF_Bilinear( uv, texSize );
    #endif

    #if( USE_LOAD == 1 )
    uv = frac( uv ) * texSize;
    #endif

    return float3( uv, mip );
}


#define CheckNonOpaqueTriangle( rayQuery, mipAndCone ) \
    { \
        /* Instance */ \
        uint instanceIndex = rayQuery.CandidateInstanceID( ) + rayQuery.CandidateGeometryIndex( ); \
        InstanceData instanceData = gIn_InstanceData[ instanceIndex ]; \
        \
        /* Transform */ \
        float3x3 mObjectToWorld = ( float3x3 )rayQuery.CandidateObjectToWorld3x4( ); \
        float3x4 mOverloaded = float3x4( instanceData.mOverloadedMatrix0, instanceData.mOverloadedMatrix1, instanceData.mOverloadedMatrix2 ); \
        if( instanceData.textureOffsetAndFlags & ( FLAG_STATIC << FLAG_FIRST_BIT ) ) \
            mObjectToWorld = ( float3x3 )mOverloaded; \
        \
        float flip = Math::Sign( instanceData.scale ) * ( rayQuery.CandidateTriangleFrontFace( ) ? -1.0 : 1.0 ); \
        \
        /* Primitive */ \
        uint primitiveIndex = instanceData.primitiveOffset + rayQuery.CandidatePrimitiveIndex( ); \
        PrimitiveData primitiveData = gIn_PrimitiveData[ primitiveIndex ]; \
        \
        float worldArea = primitiveData.worldArea * instanceData.scale * instanceData.scale; \
        \
        /* Barycentrics */ \
        float3 barycentrics; \
        barycentrics.yz = rayQuery.CandidateTriangleBarycentrics( ); \
        barycentrics.x = 1.0 - barycentrics.y - barycentrics.z; \
        \
        /* Uv */ \
        float2 uv = barycentrics.x * primitiveData.uv0 + barycentrics.y * primitiveData.uv1 + barycentrics.z * primitiveData.uv2; \
        \
        /* Normal */ \
        float3 n0 = Packing::DecodeUnitVector( primitiveData.n0, true ); \
        float3 n1 = Packing::DecodeUnitVector( primitiveData.n1, true ); \
        float3 n2 = Packing::DecodeUnitVector( primitiveData.n2, true ); \
        \
        float3 N = barycentrics.x * n0 + barycentrics.y * n1 + barycentrics.z * n2; \
        N = Geometry::RotateVector( mObjectToWorld, N ); \
        N = normalize( N * flip ); \
        \
        /* Mip level */ \
        float NoRay = abs( dot( rayQuery.WorldRayDirection( ), N ) ); \
        float a = rayQuery.CandidateTriangleRayT( ); \
        a *= mipAndCone.y; \
        a *= Math::PositiveRcp( NoRay ); \
        a *= sqrt( primitiveData.uvArea / worldArea ); \
        \
        float mip = log2( a ); \
        mip += MAX_MIP_LEVEL; \
        mip = max( mip, 0.0 ); \
        mip += mipAndCone.x; \
        \
        /* Alpha test */ \
        uint baseTexture = ( instanceData.textureOffsetAndFlags & NON_FLAG_MASK ) + 0; \
        float3 coords = GetSamplingCoords( baseTexture, uv, mip, MIP_VISIBILITY ); \
        float alpha = gIn_Textures[ baseTexture ].SAMPLE( coords ).w; \
        \
        if( alpha > 0.5 ) \
            rayQuery.CommitNonOpaqueTriangleHit( ); \
    }



GeometryProps CastRay( float3 origin, float3 direction, float Tmin, float Tmax, float2 mipAndCone, RaytracingAccelerationStructure accelerationStructure, uint instanceInclusionMask, uint rayFlags )
{
    RayDesc rayDesc;
    rayDesc.Origin = origin;
    rayDesc.Direction = direction;
    rayDesc.TMin = Tmin;
    rayDesc.TMax = Tmax;

    RayQuery< RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES > rayQuery;
    rayQuery.TraceRayInline( accelerationStructure, rayFlags, instanceInclusionMask, rayDesc );

    while( rayQuery.Proceed( ) )
        CheckNonOpaqueTriangle( rayQuery, mipAndCone );

    // TODO: reuse data if committed == candidate ( use T to check )
    GeometryProps props = ( GeometryProps )0;
    props.mip = mipAndCone.x;

    if( rayQuery.CommittedStatus( ) == COMMITTED_NOTHING )
    {
        props.hitT = INF;
        props.X = origin + direction * props.hitT;
        props.Xprev = props.X;
    }
    else
    {
        props.hitT = rayQuery.CommittedRayT( );

        // Instance
        uint instanceIndex = rayQuery.CommittedInstanceID( ) + rayQuery.CommittedGeometryIndex( );
        props.instanceIndex = instanceIndex;

        InstanceData instanceData = gIn_InstanceData[ instanceIndex ];

        // Texture offset and flags
        props.textureOffsetAndFlags = instanceData.textureOffsetAndFlags;

        // Transform
        float3x3 mObjectToWorld = ( float3x3 )rayQuery.CommittedObjectToWorld3x4( );
        float3x4 mOverloaded = float3x4( instanceData.mOverloadedMatrix0, instanceData.mOverloadedMatrix1, instanceData.mOverloadedMatrix2 ); \

        if( props.Has( FLAG_STATIC ) )
            mObjectToWorld = ( float3x3 )mOverloaded;

        float flip = Math::Sign( instanceData.scale ) * ( rayQuery.CommittedTriangleFrontFace( ) ? -1.0 : 1.0 );

        // Primitive
        uint primitiveIndex = instanceData.primitiveOffset + rayQuery.CommittedPrimitiveIndex( );
        PrimitiveData primitiveData = gIn_PrimitiveData[ primitiveIndex ];

        float worldArea = primitiveData.worldArea * instanceData.scale * instanceData.scale;

        // Barycentrics
        float3 barycentrics;
        barycentrics.yz = rayQuery.CommittedTriangleBarycentrics( );
        barycentrics.x = 1.0 - barycentrics.y - barycentrics.z;

        // Normal
        float3 n0 = Packing::DecodeUnitVector( primitiveData.n0, true );
        float3 n1 = Packing::DecodeUnitVector( primitiveData.n1, true );
        float3 n2 = Packing::DecodeUnitVector( primitiveData.n2, true );

        float3 N = barycentrics.x * n0 + barycentrics.y * n1 + barycentrics.z * n2;
        N = Geometry::RotateVector( mObjectToWorld, N );
        N = normalize( N * flip );
        props.N = -N; // TODO: why negated?

        // Curvature
        float dnSq0 = Math::LengthSquared( n0 - n1 );
        float dnSq1 = Math::LengthSquared( n1 - n2 );
        float dnSq2 = Math::LengthSquared( n2 - n0 );
        float dnSq = max( dnSq0, max( dnSq1, dnSq2 ) );
        props.curvature = sqrt( dnSq / worldArea );

        // Mip level
        float NoRay = abs( dot( direction, props.N ) );
        float a = props.hitT * mipAndCone.y;
        a *= Math::PositiveRcp( NoRay );
        a *= sqrt( primitiveData.uvArea / worldArea );

        float mip = log2( a );
        mip += MAX_MIP_LEVEL;
        mip = max( mip, 0.0 );
        props.mip += mip;

        // Uv
        props.uv = barycentrics.x * primitiveData.uv0 + barycentrics.y * primitiveData.uv1 + barycentrics.z * primitiveData.uv2;

        // Tangent
        float3 t0 = Packing::DecodeUnitVector( primitiveData.t0, true );
        float3 t1 = Packing::DecodeUnitVector( primitiveData.t1, true );
        float3 t2 = Packing::DecodeUnitVector( primitiveData.t2, true );

        float3 T = barycentrics.x * t0 + barycentrics.y * t1 + barycentrics.z * t2;
        T = Geometry::RotateVector( mObjectToWorld, T );
        T = normalize( T );
        props.T = float4( T, primitiveData.bitangentSign );

        props.X = origin + direction * props.hitT;
        
        
        // if( props.Has( FLAG_MORPH ) )
        // {
        //     MorphPrimitivePositions prev = gIn_MorphPrimitivePositionsPrev[ instanceData.morphPrimitiveOffset + rayQuery.CommittedPrimitiveIndex( ) ];
        //
        //     float3 XprevLocal = barycentrics.x * prev.pos0.xyz + barycentrics.y * prev.pos1.xyz + barycentrics.z * prev.pos2.xyz;
        //     props.Xprev = Geometry::AffineTransform( mOverloaded, XprevLocal );
        // }
        // else 
            
            if( !props.Has( FLAG_STATIC ) )
            props.Xprev = Geometry::AffineTransform( mOverloaded, props.X );
        else
            props.Xprev = props.X;
    }

    props.V = -direction;

    return props;
}

MaterialProps GetMaterialProps( GeometryProps geometryProps )
{
    MaterialProps props = ( MaterialProps )0;

    // Fast path for miss and hair
    [branch]
    if( geometryProps.IsMiss( ) )
    {
        props.Lemi = GetSkyIntensity( -geometryProps.V );

        return props;
    }
#if( RTXCR_INTEGRATION == 1 )
    else if( geometryProps.Has( FLAG_HAIR ) )
    {
        props.N = geometryProps.N;
        props.T = geometryProps.T.xyz;
        props.baseColor = gHairBaseColor.xyz * 0.25; // TODO: still not the best match in terms of energy
        props.roughness = gHairBetas.x;
        props.curvature = geometryProps.curvature;
        props.metalness = 1.0; // no diffuse lobe for hair

        return props;
    }
#endif

    uint baseTexture = geometryProps.GetBaseTexture( );
    InstanceData instanceData = gIn_InstanceData[ geometryProps.instanceIndex ];

    // Base color
    float3 coords = GetSamplingCoords( baseTexture, geometryProps.uv, geometryProps.mip, MIP_SHARP );
    float4 color = gIn_Textures[ NonUniformResourceIndex( baseTexture ) ].SAMPLE( coords );
    color.xyz *= instanceData.baseColorAndMetalnessScale.xyz;
    color.xyz *= geometryProps.Has( FLAG_TRANSPARENT ) ? 1.0 : Math::PositiveRcp( color.w ); // Correct handling of BC1 with pre-multiplied alpha
    float3 baseColor = saturate( color.xyz );

    // Roughness and metalness
    coords = GetSamplingCoords( baseTexture + 1, geometryProps.uv, geometryProps.mip, MIP_SHARP );
    float3 materialProps = gIn_Textures[ NonUniformResourceIndex( baseTexture + 1 ) ].SAMPLE( coords ).xyz;
    float roughness = saturate( materialProps.y * instanceData.emissionAndRoughnessScale.w );
    float metalness = saturate( materialProps.z * instanceData.baseColorAndMetalnessScale.w );

    // Normal
    coords = GetSamplingCoords( baseTexture + 2, geometryProps.uv * instanceData.normalUvScale, geometryProps.mip, MIP_LESS_SHARP );
    float2 packedNormal = gIn_Textures[ NonUniformResourceIndex( baseTexture + 2 ) ].SAMPLE( coords ).xy;
    float3 N = gUseNormalMap ? Geometry::TransformLocalNormal( packedNormal, geometryProps.T, geometryProps.N ) : geometryProps.N;
    float3 T = geometryProps.T.xyz;

    // Estimate curvature
    float viewZ = Geometry::AffineTransform( gWorldToView, geometryProps.X ).z;
    float pixelSize = gUnproject * lerp( abs( viewZ ), 1.0, abs( gOrthoMode ) );
    float localCurvature = length( Geometry::UnpackLocalNormal( packedNormal ).xy ) * float( gUseNormalMap );
    localCurvature /= pixelSize;

    // Emission
    coords = GetSamplingCoords( baseTexture + 3, geometryProps.uv, geometryProps.mip, MIP_VISIBILITY );
    float3 Lemi = gIn_Textures[ NonUniformResourceIndex( baseTexture + 3 ) ].SAMPLE( coords ).xyz;
    Lemi *= instanceData.emissionAndRoughnessScale.xyz;
    Lemi *= ( baseColor + 0.01 ) / ( max( baseColor, max( baseColor, baseColor ) ) + 0.01 );

    [flatten]
    if( geometryProps.Has( FLAG_FORCED_EMISSION ) )
    {
        Lemi = geometryProps.GetForcedEmissionColor( );
        baseColor = 0.0;
    }

    Lemi *= gEmissionIntensity;

    // Material overrides
    [flatten]
    if( gForcedMaterial == MATERIAL_GYPSUM )
    {
        roughness = 1.0;
        baseColor = 0.5;
        metalness = 0.0;
    }
    else if( gForcedMaterial == MATERIAL_COBALT )
    {
        roughness = pow( saturate( baseColor.x * baseColor.y * baseColor.z ), 0.33333 );
        baseColor = float3( 0.672411, 0.637331, 0.585456 );
        metalness = 1.0;

        #if( USE_ANOTHER_COBALT == 1 )
            roughness = pow( saturate( roughness - 0.1 ), 0.25 ) * 0.3 + 0.07;
        #endif
    }

    metalness = gMetalnessOverride == 0.0 ? metalness : gMetalnessOverride;
    roughness = gRoughnessOverride == 0.0 ? roughness : gRoughnessOverride;

    #if( USE_PUDDLES == 1 )
        roughness *= Math::SmoothStep( 0.6, 0.8, length( frac( geometryProps.uv ) * 2.0 - 1.0 ) );
    #endif

    #if( USE_RANDOMIZED_ROUGHNESS == 1 )
        float2 noise = ( frac( sin( dot( geometryProps.uv, float2( 12.9898, 78.233 ) * 2.0 ) ) * 43758.5453 ) );
        float noise01 = abs( noise.x + noise.y ) * 0.5;
        roughness *= 1.0 + ( noise01 * 2.0 - 1.0 ) * 0.25;
    #endif

    roughness = saturate( roughness );
    metalness = saturate( metalness );

    // Transform to diffuse material if emission is here
    float emissionLevel = Color::Luminance( Lemi );
    emissionLevel = saturate( emissionLevel * 50.0 );

    metalness = lerp( metalness, 0.0, emissionLevel );
    roughness = lerp( roughness, 1.0, emissionLevel );

    props.Lemi = Lemi;
    props.N = N;
    props.T = T;
    props.baseColor = baseColor;
    props.roughness = roughness;
    props.metalness = metalness;
    props.curvature = geometryProps.curvature + localCurvature;

    return props;
}
