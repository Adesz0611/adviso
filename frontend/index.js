import { mat4, vec3 } from 'https://esm.sh/wgpu-matrix';

async function loadMesh(url) {
    const response = await fetch(url);
    const buffer = await response.arrayBuffer();

    const headerView = new DataView(buffer, 0, 8);
    const numTriangles = headerView.getUint32(0, true);
    const varDim = headerView.getUint32(4, true);

    const numVertices = numTriangles * 3;
    const bytesPerFloat = 4;

    const posOffset = 8;
    const posByteLen = numVertices * 3 * bytesPerFloat;

    const normOffset = posOffset + posByteLen;
    const normByteLen = numVertices * 3 * bytesPerFloat;

    const varOffset = normOffset + normByteLen;
    const varByteLen = numVertices * varDim * bytesPerFloat;

    console.log(`numVertices: ${numVertices}`);
    const positions = new Float32Array(buffer, posOffset, numVertices * 3);
    const normals = new Float32Array(buffer, normOffset, numVertices * 3);
    const variable = new Float32Array(buffer, varOffset, numVertices * varDim);

    console.log(`numTriangles: ${numTriangles}`);
    console.log(`varDim: ${varDim}`);

    // BOUNDING BOX calculate
    let minX =  Infinity, minY =  Infinity, minZ =  Infinity;
    let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;

    for (let i = 0; i < positions.length; i += 3) {
        const x = positions[i];
        const y = positions[i + 1];
        const z = positions[i + 2];

        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
        if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
    }

    let varMin = 0.0, varMax = 1.0;
    for (let i = 0; i < variable.length; ++i) {
        const value = variable[i];

        if (value < varMin) varMin = value;
        if (value > varMax) varMax = value;
    }

    console.log(`varMin: ${varMin} varMax: ${varMax}`);
    document.getElementById('tri-val').innerHTML = numTriangles;

    return {
        numVertices,
        varDim,
        positions,
        normals,
        variable,
        minX, maxX,
        minY, maxY,
        minZ, maxZ,
        varMin, varMax,
    };
}

const mesh = await loadMesh('encas/slice.adbin');
const scaleFactor = 1 / Math.max(mesh.maxX - mesh.minX, mesh.maxY - mesh.minY, mesh.maxZ - mesh.minZ);
const centerX = (mesh.minX + mesh.maxX) / 2;
const centerY = (mesh.minY + mesh.maxY) / 2;
const centerZ = (mesh.minZ + mesh.maxZ) / 2;

const canvas = document.querySelector("canvas");
const canvasWrapper = document.getElementById("canvas-wrapper");
const fpsDisplay = document.getElementById("fps-val");

let lastTime = performance.now();
let frameCount = 0;
let fps = 0;

const target = vec3.create(0, 0, 0);
const up = vec3.create(0, 1, 0);

let isDragging = false;

let lastMouseX = 0;
let lastMouseY = 0;

const rotationSpeed = 0.0035;
const zoomSpeed = 0.001;

const cameraMinDistance = 0.1;
const cameraMaxDistance = 10000;

let rotationX = 0;
let rotationY = 0;

let cameraRadius = 2;
let eye = vec3.create(0, 0, cameraRadius);

const fov = 45;
const fov_in_rad = (fov * Math.PI) / 180;
const initialAspect = (canvas.clientWidth && canvas.clientHeight)
    ? canvas.clientWidth / canvas.clientHeight
    : canvas.width / canvas.height;
const zNear = 0.1;
const zFar = 1000;

const projectionMatrix = mat4.perspective(fov_in_rad, initialAspect, zNear, zFar);


let viewMatrix = mat4.lookAt(eye, target, up);

let modelMatrix = mat4.identity();
mat4.scale(modelMatrix, [scaleFactor, scaleFactor, scaleFactor], modelMatrix);
mat4.translate(modelMatrix, [-centerX, -centerY, -centerZ], modelMatrix);

canvas.addEventListener('mousedown', (event) => {
    if (event.button == 0) {
        isDragging = true;

        lastMouseX = event.clientX;
        lastMouseY = event.clientY;
    }
});


window.addEventListener('mouseup', (event) => {
    isDragging = false;
});


let depthTexture;
let msaaTexture;

//mat4.rotateY(modelMatrix, 0.5, modelMatrix);
//mat4.translate(modelMatrix, vec3.create(0, 0, -1), modelMatrix);

const PV = mat4.create();
mat4.multiply(projectionMatrix, viewMatrix, PV);

if (!navigator.gpu) {
    alert("WebGPU not supported on this browser.");
}

const adapter = await navigator.gpu.requestAdapter();
if (!adapter) {
    throw new Error("No appropriate GPUAdapter found.");
}

const device = await adapter.requestDevice();

const context = canvas.getContext("webgpu");
const canvasFormat = navigator.gpu.getPreferredCanvasFormat();
context.configure({
    device: device,
    format: canvasFormat,
});


function createStaticBuffer(data, usage) {
    const buffer = device.createBuffer({
        size: data.byteLength,
        usage: usage | GPUBufferUsage.COPY_DST,
        mappedAtCreation: true,
    });

    new data.constructor(buffer.getMappedRange()).set(data);
    buffer.unmap();
    return buffer;
}

const posBuffer  = createStaticBuffer(mesh.positions, GPUBufferUsage.VERTEX);
const normBuffer = createStaticBuffer(mesh.normals,   GPUBufferUsage.VERTEX);
const varBuffer  = createStaticBuffer(mesh.variable,  GPUBufferUsage.VERTEX);


// TEXTURE FOR VARIABLE COLORING
const width = 256;
const textureData = new Uint8Array(width * 4);
for (let i = 0; i < width; i++) {
    const t = i / (width - 1);
    textureData[i*4 + 0] = Math.floor(255 * t);
    textureData[i*4 + 1] = Math.floor(255 * (1-Math.abs(t-0.5)*2));
    textureData[i*4 + 2] = Math.floor(255 * (1-t));
    textureData[i*4 + 3] = 255;
}

const colorMapTexture = device.createTexture({
    size: [width, 1, 1],
    format: 'rgba8unorm',
    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
});

device.queue.writeTexture(
    { texture: colorMapTexture },
    textureData,
    { bytesPerRow: width * 4 },
    { width: width, height: 1 }
);

const colormapSampler = device.createSampler({
    magFilter: 'linear',
    minFilter: 'linear',
    addressModeU: 'clamp-to-edge',
    addressModeV: 'clamp-to-edge',
});

// UNIFORM BUFFERS

const PVBuffer = device.createBuffer({
    label: "Projection View Matrix Buffer",
    size: PV.byteLength,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
});

device.queue.writeBuffer(PVBuffer, 0, PV);

const viewBuffer = device.createBuffer({
    label: "View Matrix Buffer",
    size: viewMatrix.byteLength,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
});

device.queue.writeBuffer(viewBuffer, 0, viewMatrix);

const rangeData = new Float32Array([mesh.varMin, mesh.varMax]);
const rangeBuffer = device.createBuffer({
    label: "Range Uniform Buffer",
    size: rangeData.byteLength, 
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
});
device.queue.writeBuffer(rangeBuffer, 0, rangeData);

function updateCamera() {
    eye[0] = cameraRadius * Math.cos(rotationY) * Math.sin(rotationX);
    eye[1] = cameraRadius * Math.sin(rotationY);
    eye[2] = cameraRadius * Math.cos(rotationY) * Math.cos(rotationX);

    mat4.lookAt(eye, target, up, viewMatrix);
    mat4.multiply(projectionMatrix, viewMatrix, PV);
    
    device.queue.writeBuffer(PVBuffer, 0, PV);
    device.queue.writeBuffer(viewBuffer, 0, viewMatrix);
}

window.addEventListener('wheel', (event) => {
    event.preventDefault();

    cameraRadius += event.deltaY * cameraRadius * zoomSpeed;

    cameraRadius = Math.max(cameraMinDistance, Math.min(cameraMaxDistance, cameraRadius));

    updateCamera();

}, { passive: false });

window.addEventListener('mousemove', (event) => {
    if (isDragging) {
        
        const deltaX = event.clientX - lastMouseX;
        const deltaY = event.clientY - lastMouseY;

        rotationX -= deltaX * rotationSpeed;
        rotationY += deltaY * rotationSpeed;

        lastMouseX = event.clientX;
        lastMouseY = event.clientY;

        if (rotationY > 1.57) {
            rotationY = 1.57;
        }
        if (rotationY < -1.57) {
            rotationY = -1.57;
        }

        updateCamera();
    }
});

const modelBuffer = device.createBuffer({
    label: "Model Matrix Buffer",
    size: modelMatrix.byteLength,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
});
device.queue.writeBuffer(modelBuffer, 0, modelMatrix);

const vertexBufferLayout = {
    arrayStride: 12,
    attributes: [{
        format: "float32x3",
        offset: 0,
        shaderLocation: 0, // Position, see vertex shader
    }],
};

const normalBufferLayout = {
    arrayStride: 12,
    attributes: [{
        format: "float32x3",
        offset: 0,
        shaderLocation: 1, // Position, see vertex shader
    }],
};

const variableBufferLayout = {
    arrayStride: 4,
    attributes: [{
        format: "float32",
        offset: 0,
        shaderLocation: 2,
    }],
}

const cubeShaderModule = device.createShaderModule({
    label: "Cube Shader",
    code: `
        struct VertexOutput {
            @builtin(position) position: vec4f,
            @location(0) viewNormal: vec3f,
            @location(1) normalizedVal: f32,
        };

        struct Range {
            min: f32,
            max: f32,
        };

        @group(0) @binding(0) var<uniform> model: mat4x4f;
        @group(0) @binding(1) var<uniform> PV: mat4x4f;
        @group(0) @binding(2) var<uniform> view: mat4x4f;

        @group(0) @binding(3) var<uniform> range: Range;
        @group(0) @binding(4) var colormap: texture_2d<f32>;
        @group(0) @binding(5) var mySampler: sampler;


        @vertex
        fn vertexMain(@location(0) pos: vec3f, @location(1) normal: vec3f, @location(2) value: f32) -> VertexOutput {
            var output: VertexOutput;

            let viewMatrix = view * model;
            let transformedNormal = viewMatrix * vec4f(normal, 0.0);

            output.position = PV * model * vec4f(pos, 1);
            output.viewNormal = transformedNormal.xyz;
            output.normalizedVal = clamp((value - range.min) / (range.max - range.min), 0.0, 1.0);

            return output;
        }

        @fragment
        fn fragmentMain(input: VertexOutput) -> @location(0) vec4f {
            let baseColor = textureSample(colormap, mySampler, vec2f(input.normalizedVal, 0.5)).rgb;


            let N = normalize(input.viewNormal);

            let lightDir = vec3f(0.0, 0.0, 1.0);
            let diffuse  = abs(dot(N, lightDir));

            let ambient = 0.2;

            let finalColor = baseColor * (diffuse + ambient);

            return vec4f(finalColor, 1);
        }
    `
});

const bindGroupLayout = device.createBindGroupLayout({
    label: "Cube Bind Group Layout",
    entries: [
        {
            binding: 0,
            visibility: GPUShaderStage.VERTEX,
            buffer: {},
        },
        {
            binding: 1,
            visibility: GPUShaderStage.VERTEX,
            buffer: {},
        },
        {
            binding: 2,
            visibility: GPUShaderStage.VERTEX,
            buffer: {},
        },
        {
            binding: 3,
            visibility: GPUShaderStage.VERTEX,
            buffer: {},
        },
        {
            binding: 4,
            visibility: GPUShaderStage.FRAGMENT,
            texture: { viewDimension: '2d', },
        },
        {
            binding: 5,
            visibility: GPUShaderStage.FRAGMENT,
            sampler: {},
        },
    ],
});

const pipelineLayout = device.createPipelineLayout({
    label: "Cube Pipeline Layout",
    bindGroupLayouts: [ bindGroupLayout ],
});

const cellPipeline = device.createRenderPipeline({
    label: "Cube Render Pipeline",
    layout: pipelineLayout,
    vertex: {
        module: cubeShaderModule,
        entryPoint: "vertexMain",
        buffers: [ vertexBufferLayout, normalBufferLayout, variableBufferLayout ],
    },
    fragment: {
        module: cubeShaderModule,
        entryPoint: "fragmentMain",
        targets: [{
            format: canvasFormat
        }]
    },
    primitive: {
        cullMode: "none",
        frontFace: "ccw",
        topology: "triangle-list",
    },
    multisample: {
        count: 4,
    },
    depthStencil: {
        format: "depth24plus",
        depthWriteEnabled: true,
        depthCompare: "less",
    },
});

const bindGroup = device.createBindGroup({
    label: "Cube Bind Group",
    layout: bindGroupLayout,
    entries: [
        {
            binding: 0,
            resource: { buffer: modelBuffer },
        },
        {
            binding: 1,
            resource: { buffer: PVBuffer },
        },
        {
            binding: 2,
            resource: { buffer: viewBuffer },
        },
        {
            binding: 3,
            resource: { buffer: rangeBuffer },
        },
        {
            binding: 4,
            resource: colorMapTexture.createView(),
        },
        {
            binding: 5,
            resource: colormapSampler,
        },
    ],
});

function updateGrid() {
    //mat4.rotateY(modelMatrix, 0.01, modelMatrix);
    //device.queue.writeBuffer(modelBuffer, 0, modelMatrix);

    const now = performance.now();
    ++frameCount;

    if (now - lastTime >= 1000) {
        fps = frameCount;

        fpsDisplay.innerText = `${fps}`;

        frameCount = 0;
        lastTime = now;
    }

    const encoder = device.createCommandEncoder();

    const pass = encoder.beginRenderPass({
        colorAttachments: [{
            view: msaaTexture.createView(),
            resolveTarget: context.getCurrentTexture().createView(),
            loadOp: "clear",
            clearValue: [0.1, 0.1, 0.1, 1],
            storeOp: "discard",
        }],
        depthStencilAttachment: {
            view: depthTexture.createView(),
            depthClearValue: 1.0,
            depthLoadOp: "clear",
            depthStoreOp: "discard",
        }
    });

    pass.setPipeline(cellPipeline);
    pass.setVertexBuffer(0, posBuffer);
    pass.setVertexBuffer(1, normBuffer);
    pass.setVertexBuffer(2, varBuffer);
    pass.setBindGroup(0, bindGroup);
    pass.draw(mesh.numVertices);

    pass.end();

    device.queue.submit([encoder.finish()]);

    requestAnimationFrame(updateGrid);
}

function resize() {
    // 1. Lekérjük a canvas TÉNYLEGES megjelenítési méretét (CSS)
    const displayWidth = canvasWrapper.clientWidth;
    const displayHeight = canvasWrapper.clientHeight;

    // Ha véletlenül 0 (pl. minimalizált ablak), ne csináljunk semmit
    if (displayWidth === 0 || displayHeight === 0) return;

    // 2. Számoljuk a fizikai pixeleket (Retina kijelzők miatt)
    //const deviceRatio = window.devicePixelRatio || 1;
    const deviceRatio = Math.min(window.devicePixelRatio, 1.5);
    const newWidth = Math.floor(displayWidth * deviceRatio);
    const newHeight = Math.floor(displayHeight * deviceRatio);

    // 3. Csak akkor méretezzük át a buffert, ha tényleg változott
    if (canvas.width !== newWidth || canvas.height !== newHeight) {
        canvas.width = newWidth;
        canvas.height = newHeight;

        console.log(`Resize: ${newWidth}x${newHeight}, Aspect: ${newWidth/newHeight}`);

        // Textúrák eldobása és újragenerálása
        if (depthTexture) depthTexture.destroy();
        if (msaaTexture) msaaTexture.destroy();

        msaaTexture = device.createTexture({
            size: [canvas.width, canvas.height],
            sampleCount: 4,
            format: canvasFormat,
            usage: GPUTextureUsage.RENDER_ATTACHMENT,
        });

        depthTexture = device.createTexture({
            size: [canvas.width, canvas.height],
            format: 'depth24plus',
            sampleCount: 4,
            usage: GPUTextureUsage.RENDER_ATTACHMENT,
        });
    }

    // 4. A LÉNYEG: A képarányt a pufferméretből számoljuk!
    const aspect = canvas.width / canvas.height;

    // Frissítjük a vetítési mátrixot (ez javítja a torzítást)
    // fov_in_rad globális változó, aspect dinamikus, zNear/zFar globális
    mat4.perspective(fov_in_rad, aspect, zNear, zFar, projectionMatrix);
    
    // Összeszorozzuk a View mátrixszal és feltöltjük a GPU-ra
    mat4.multiply(projectionMatrix, viewMatrix, PV);
    device.queue.writeBuffer(PVBuffer, 0, PV);
}

const observer = new ResizeObserver(() => {
    resize();
});
observer.observe(canvasWrapper);
resize();

requestAnimationFrame(updateGrid);

