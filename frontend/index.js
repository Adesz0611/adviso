import { mat4, vec3 } from 'https://esm.sh/wgpu-matrix';

const MESH_PACKET_MAGIC = 0x53564441;
const MESH_PACKET_HEADER_BYTES = 56;
const DEFAULT_SLICE_RESOLUTION = 1500;

const PACKET_TYPE_SURFACE = 1;
const PACKET_TYPE_SLICE = 2;

const ui = {
    canvas: document.querySelector('canvas'),
    casePath: document.getElementById('case-path'),
    loadCaseBtn: document.getElementById('load-case-btn'),
    variableSelect: document.getElementById('variable-select'),
    timeSlider: document.getElementById('time-slider'),
    timeLabel: document.getElementById('time-label'),
    surfaceBtn: document.getElementById('surface-btn'),
    sliceBtn: document.getElementById('slice-btn'),
    sliceInput: document.getElementById('slice-input'),
    status: document.getElementById('status'),
    boundsMin: document.getElementById('bounds-min'),
    boundsMax: document.getElementById('bounds-max'),
    scalarRange: document.getElementById('scalar-range'),
    fps: document.getElementById('fps-value'),
    mode: document.getElementById('mode-value'),
    triangles: document.getElementById('triangles-value'),
    vertices: document.getElementById('vertices-value'),
};

const state = {
    socket: null,
    socketReady: false,
    caseInfo: null,
    currentMode: 'idle',
    lastSliceRequest: { axis: 'z', value: 0 },
};

function setStatus(message, kind = 'info') {
    ui.status.textContent = message;
    ui.status.className = kind === 'info' ? '' : kind;
}

function formatNumber(value) {
    if (!Number.isFinite(value)) return '-';
    if (Math.abs(value) >= 1000 || Math.abs(value) < 0.001) {
        return value.toExponential(3);
    }
    return value.toFixed(4);
}

function formatVec3(values) {
    return values.map((value) => formatNumber(value)).join(', ');
}

function getSelectedVariableIndex() {
    if (ui.variableSelect.disabled || ui.variableSelect.value === '') return -1;
    return Number.parseInt(ui.variableSelect.value, 10);
}

function getSelectedTimeIndex() {
    return Number.parseInt(ui.timeSlider.value, 10) || 0;
}

function updateTimeLabel() {
    const current = getSelectedTimeIndex();
    const max = Number.parseInt(ui.timeSlider.max, 10) || 0;
    ui.timeLabel.textContent = `${current} / ${max}`;
}

function getActiveTimeCount() {
    if (!state.caseInfo) return 1;

    const variableIndex = getSelectedVariableIndex();
    if (variableIndex >= 0) {
        const variable = state.caseInfo.variables.find((item) => item.index === variableIndex);
        if (variable) return Math.max(variable.timeCount, 1);
    }

    return Math.max(state.caseInfo.geometryTimeCount ?? 1, 1);
}

function updateControlState() {
    const hasCase = Boolean(state.caseInfo);
    const hasVariables = hasCase && state.caseInfo.variables.length > 0;

    ui.variableSelect.disabled = !hasVariables;
    ui.timeSlider.disabled = !hasCase;
    ui.surfaceBtn.disabled = !hasCase || !state.socketReady;
    ui.sliceBtn.disabled = !hasCase || !state.socketReady;
    ui.sliceInput.disabled = !hasCase;

    const timeCount = getActiveTimeCount();
    ui.timeSlider.max = String(Math.max(timeCount - 1, 0));

    if (getSelectedTimeIndex() >= timeCount) {
        ui.timeSlider.value = '0';
    }

    updateTimeLabel();
}

function populateVariables(caseInfo) {
    ui.variableSelect.innerHTML = '';

    if (!caseInfo.variables.length) {
        const option = document.createElement('option');
        option.value = '';
        option.textContent = 'No scalar field';
        ui.variableSelect.append(option);
        return;
    }

    for (const variable of caseInfo.variables) {
        const option = document.createElement('option');
        option.value = String(variable.index);
        option.textContent = variable.name;
        ui.variableSelect.append(option);
    }
}

function parseSliceInput(text) {
    const trimmed = text.trim();
    const explicit = trimmed.match(/^([xyzXYZ])\s*=\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)$/);
    if (explicit) {
        return {
            axis: explicit[1].toLowerCase(),
            value: Number.parseFloat(explicit[2]),
        };
    }

    const numeric = Number.parseFloat(trimmed);
    if (Number.isFinite(numeric)) {
        return {
            axis: 'z',
            value: numeric,
        };
    }

    return null;
}

function sendJsonMessage(payload) {
    if (!state.socketReady || !state.socket) {
        setStatus('Websocket is not connected.', 'error');
        return false;
    }

    state.socket.send(JSON.stringify(payload));
    return true;
}

function requestSurface() {
    state.currentMode = 'surface';
    ui.mode.textContent = 'surface';
    sendJsonMessage({
        type: 'request_surface',
        variableIndex: getSelectedVariableIndex(),
        timeIndex: getSelectedTimeIndex(),
    });
}

function requestSlice() {
    const slice = parseSliceInput(ui.sliceInput.value);
    if (!slice) {
        setStatus('Slice format must be x=..., y=... or z=....', 'error');
        return;
    }

    state.lastSliceRequest = slice;
    state.currentMode = 'slice';
    ui.mode.textContent = 'slice';
    sendJsonMessage({
        type: 'request_slice',
        variableIndex: getSelectedVariableIndex(),
        timeIndex: getSelectedTimeIndex(),
        axis: slice.axis,
        value: slice.value,
        resolution: DEFAULT_SLICE_RESOLUTION,
    });
}

function requestCurrentView() {
    if (!state.caseInfo) return;

    if (state.currentMode === 'idle') {
        return;
    }

    if (state.currentMode === 'slice') {
        requestSlice();
    } else {
        requestSurface();
    }
}

function updateDatasetInfo(mesh) {
    ui.boundsMin.textContent = formatVec3(mesh.boundsMin);
    ui.boundsMax.textContent = formatVec3(mesh.boundsMax);
    ui.scalarRange.textContent = `${formatNumber(mesh.scalarMin)} .. ${formatNumber(mesh.scalarMax)}`;
    ui.triangles.textContent = mesh.triangleCount.toLocaleString();
    ui.vertices.textContent = mesh.vertexCount.toLocaleString();
    ui.mode.textContent = mesh.kind;
}

function parseMeshPacket(buffer) {
    const view = new DataView(buffer);
    const magic = view.getUint32(0, true);
    if (magic !== MESH_PACKET_MAGIC) {
        throw new Error('Unexpected mesh packet magic.');
    }

    const payloadType = view.getUint32(8, true);
    const vertexCount = view.getUint32(12, true);
    const indexCount = view.getUint32(16, true);
    const triangleCount = view.getUint32(20, true);

    const boundsMin = [
        view.getFloat32(24, true),
        view.getFloat32(28, true),
        view.getFloat32(32, true),
    ];

    const boundsMax = [
        view.getFloat32(36, true),
        view.getFloat32(40, true),
        view.getFloat32(44, true),
    ];

    const scalarMin = view.getFloat32(48, true);
    const scalarMax = view.getFloat32(52, true);

    let byteOffset = MESH_PACKET_HEADER_BYTES;
    const positions = new Float32Array(buffer, byteOffset, vertexCount * 3);
    byteOffset += positions.byteLength;

    const indices = new Uint32Array(buffer, byteOffset, indexCount);
    byteOffset += indices.byteLength;

    const faceNormals = new Float32Array(buffer, byteOffset, triangleCount * 3);
    byteOffset += faceNormals.byteLength;

    const scalars = new Float32Array(buffer, byteOffset, vertexCount);

    return {
        kind: payloadType === PACKET_TYPE_SLICE ? 'slice' : 'surface',
        vertexCount,
        indexCount,
        triangleCount,
        boundsMin,
        boundsMax,
        scalarMin,
        scalarMax,
        positions,
        indices,
        faceNormals,
        scalars,
    };
}

function expandMeshForRendering(mesh) {
    const vertexCount = mesh.indexCount;
    const positions = new Float32Array(vertexCount * 3);
    const scalars = new Float32Array(vertexCount);
    const normals = new Float32Array(vertexCount * 3);

    for (let tri = 0; tri < mesh.triangleCount; tri += 1) {
        const nx = mesh.faceNormals[tri * 3 + 0];
        const ny = mesh.faceNormals[tri * 3 + 1];
        const nz = mesh.faceNormals[tri * 3 + 2];

        for (let corner = 0; corner < 3; corner += 1) {
            const srcIndex = mesh.indices[tri * 3 + corner];
            const dstIndex = tri * 3 + corner;

            positions[dstIndex * 3 + 0] = mesh.positions[srcIndex * 3 + 0];
            positions[dstIndex * 3 + 1] = mesh.positions[srcIndex * 3 + 1];
            positions[dstIndex * 3 + 2] = mesh.positions[srcIndex * 3 + 2];

            scalars[dstIndex] = mesh.scalars[srcIndex];

            normals[dstIndex * 3 + 0] = nx;
            normals[dstIndex * 3 + 1] = ny;
            normals[dstIndex * 3 + 2] = nz;
        }
    }

    return {
        positions,
        scalars,
        normals,
        drawVertexCount: vertexCount,
    };
}

async function createRenderer() {
    if (!navigator.gpu) {
        throw new Error('WebGPU is not available in this browser.');
    }

    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) {
        throw new Error('No suitable GPU adapter found.');
    }

    const device = await adapter.requestDevice();
    const context = ui.canvas.getContext('webgpu');
    const format = navigator.gpu.getPreferredCanvasFormat();
    const sampleCount = 1;

    context.configure({
        device,
        format,
        alphaMode: 'opaque',
    });

    const depthFormat = 'depth24plus';
    let depthTexture = null;
    let msaaTexture = null;

    const modelMatrix = mat4.identity();
    const viewMatrix = mat4.identity();
    const projectionMatrix = mat4.identity();
    const viewProjectionMatrix = mat4.identity();

    const camera = {
        rotationX: 0.65,
        rotationY: 0.35,
        radius: 2.4,
        minRadius: 0.35,
        maxRadius: 24,
        target: vec3.create(0, 0, 0),
        up: vec3.create(0, 1, 0),
        eye: vec3.create(0, 0, 0),
    };

    const modelBuffer = device.createBuffer({
        size: modelMatrix.byteLength,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });

    const viewProjectionBuffer = device.createBuffer({
        size: viewProjectionMatrix.byteLength,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });

    const rangeBuffer = device.createBuffer({
        size: 16,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });

    const colormapPixels = new Uint8Array(256 * 4);
    for (let i = 0; i < 256; i += 1) {
        const t = i / 255;
        colormapPixels[i * 4 + 0] = Math.floor(255 * Math.min(1, Math.max(0, 1.4 * t)));
        colormapPixels[i * 4 + 1] = Math.floor(255 * (1 - Math.abs(t - 0.5) * 1.8));
        colormapPixels[i * 4 + 2] = Math.floor(255 * (1 - t * 0.9));
        colormapPixels[i * 4 + 3] = 255;
    }

    const colormapTexture = device.createTexture({
        size: [256, 1, 1],
        format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });

    device.queue.writeTexture(
        { texture: colormapTexture },
        colormapPixels,
        { bytesPerRow: 256 * 4 },
        { width: 256, height: 1 }
    );

    const colormapSampler = device.createSampler({
        magFilter: 'linear',
        minFilter: 'linear',
        addressModeU: 'clamp-to-edge',
        addressModeV: 'clamp-to-edge',
    });

    const shader = device.createShaderModule({
        label: 'ADVISO Shader',
        code: `
            @group(0) @binding(0) var<uniform> model: mat4x4f;
            @group(0) @binding(1) var<uniform> viewProjection: mat4x4f;
            @group(0) @binding(2) var<uniform> scalarRange: vec4f;
            @group(0) @binding(3) var colormap: texture_2d<f32>;
            @group(0) @binding(4) var colormapSampler: sampler;

            struct VertexOut {
                @builtin(position) position: vec4f,
                @location(0) scalar: f32,
                @location(1) normal: vec3f,
            };

            @vertex
            fn vertexMain(
                @location(0) position: vec3f,
                @location(1) scalar: f32,
                @location(2) normal: vec3f,
            ) -> VertexOut {
                var out: VertexOut;
                let worldPosition = model * vec4f(position, 1.0);
                out.position = viewProjection * worldPosition;
                out.scalar = scalar;
                out.normal = normalize((model * vec4f(normal, 0.0)).xyz);
                return out;
            }

            @fragment
            fn fragmentMain(input: VertexOut) -> @location(0) vec4f {
                let rangeWidth = max(scalarRange.y - scalarRange.x, 1e-6);
                let t = clamp((input.scalar - scalarRange.x) / rangeWidth, 0.0, 1.0);
                let base = textureSample(colormap, colormapSampler, vec2f(t, 0.5)).rgb;

                let lightDir = normalize(vec3f(0.45, 0.7, 0.6));
                let diffuse = abs(dot(normalize(input.normal), lightDir));
                let shaded = base * (0.22 + 0.78 * diffuse);

                return vec4f(shaded, 1.0);
            }
        `,
    });

    const shaderInfo = await shader.getCompilationInfo();
    if (shaderInfo.messages.some((message) => message.type === 'error')) {
        const details = shaderInfo.messages
            .map((message) => `${message.type}: ${message.message}`)
            .join('\n');
        throw new Error(`WGSL shader compilation failed:\n${details}`);
    }

    const bindGroupLayout = device.createBindGroupLayout({
        entries: [
            { binding: 0, visibility: GPUShaderStage.VERTEX, buffer: {} },
            { binding: 1, visibility: GPUShaderStage.VERTEX, buffer: {} },
            { binding: 2, visibility: GPUShaderStage.FRAGMENT, buffer: {} },
            { binding: 3, visibility: GPUShaderStage.FRAGMENT, texture: {} },
            { binding: 4, visibility: GPUShaderStage.FRAGMENT, sampler: {} },
        ],
    });

    const pipeline = device.createRenderPipeline({
        layout: device.createPipelineLayout({ bindGroupLayouts: [bindGroupLayout] }),
        vertex: {
            module: shader,
            entryPoint: 'vertexMain',
            buffers: [
                {
                    arrayStride: 12,
                    attributes: [{ shaderLocation: 0, format: 'float32x3', offset: 0 }],
                },
                {
                    arrayStride: 4,
                    attributes: [{ shaderLocation: 1, format: 'float32', offset: 0 }],
                },
                {
                    arrayStride: 12,
                    attributes: [{ shaderLocation: 2, format: 'float32x3', offset: 0 }],
                },
            ],
        },
        fragment: {
            module: shader,
            entryPoint: 'fragmentMain',
            targets: [{ format }],
        },
        primitive: {
            topology: 'triangle-list',
            cullMode: 'none',
            frontFace: 'ccw',
        },
        depthStencil: {
            format: depthFormat,
            depthWriteEnabled: true,
            depthCompare: 'less',
        },
        multisample: {
            count: sampleCount,
        },
    });

    const rendererState = {
        positionBuffer: null,
        scalarBuffer: null,
        normalBuffer: null,
        bindGroup: null,
        drawVertexCount: 0,
        triangleCount: 0,
        frameCount: 0,
        lastFpsTime: performance.now(),
        dragging: false,
        lastPointerX: 0,
        lastPointerY: 0,
        renderScheduled: false,
    };

    function destroyGpuBuffer(buffer) {
        if (buffer) buffer.destroy();
    }

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

    function updateMatrices() {
        camera.eye[0] = camera.radius * Math.cos(camera.rotationY) * Math.sin(camera.rotationX);
        camera.eye[1] = camera.radius * Math.sin(camera.rotationY);
        camera.eye[2] = camera.radius * Math.cos(camera.rotationY) * Math.cos(camera.rotationX);

        mat4.lookAt(camera.eye, camera.target, camera.up, viewMatrix);
        mat4.multiply(projectionMatrix, viewMatrix, viewProjectionMatrix);

        device.queue.writeBuffer(modelBuffer, 0, modelMatrix);
        device.queue.writeBuffer(viewProjectionBuffer, 0, viewProjectionMatrix);
    }

    function resize() {
        const width = Math.max(1, ui.canvas.clientWidth);
        const height = Math.max(1, ui.canvas.clientHeight);
        const pixelRatio = Math.min(window.devicePixelRatio || 1, 1.75);
        const physicalWidth = Math.max(1, Math.floor(width * pixelRatio));
        const physicalHeight = Math.max(1, Math.floor(height * pixelRatio));

        if (ui.canvas.width !== physicalWidth || ui.canvas.height !== physicalHeight) {
            ui.canvas.width = physicalWidth;
            ui.canvas.height = physicalHeight;

            if (depthTexture) depthTexture.destroy();
            if (msaaTexture) msaaTexture.destroy();

            depthTexture = device.createTexture({
                size: [physicalWidth, physicalHeight],
                format: depthFormat,
                sampleCount,
                usage: GPUTextureUsage.RENDER_ATTACHMENT,
            });

            if (sampleCount > 1) {
                msaaTexture = device.createTexture({
                    size: [physicalWidth, physicalHeight],
                    format,
                    sampleCount,
                    usage: GPUTextureUsage.RENDER_ATTACHMENT,
                });
            } else {
                msaaTexture = null;
            }
        }

        mat4.perspective((45 * Math.PI) / 180, physicalWidth / physicalHeight, 0.1, 100.0, projectionMatrix);
        updateMatrices();
        requestRender();
    }

    function setMesh(mesh) {
        destroyGpuBuffer(rendererState.positionBuffer);
        destroyGpuBuffer(rendererState.scalarBuffer);
        destroyGpuBuffer(rendererState.normalBuffer);

        const expandedMesh = expandMeshForRendering(mesh);

        rendererState.positionBuffer = createStaticBuffer(expandedMesh.positions, GPUBufferUsage.VERTEX);
        rendererState.scalarBuffer = createStaticBuffer(expandedMesh.scalars, GPUBufferUsage.VERTEX);
        rendererState.normalBuffer = createStaticBuffer(expandedMesh.normals, GPUBufferUsage.VERTEX);
        rendererState.drawVertexCount = expandedMesh.drawVertexCount;
        rendererState.triangleCount = mesh.triangleCount;

        device.queue.writeBuffer(rangeBuffer, 0, new Float32Array([mesh.scalarMin, mesh.scalarMax, 0, 0]));

        const sizeX = mesh.boundsMax[0] - mesh.boundsMin[0];
        const sizeY = mesh.boundsMax[1] - mesh.boundsMin[1];
        const sizeZ = mesh.boundsMax[2] - mesh.boundsMin[2];
        const maxSize = Math.max(sizeX, sizeY, sizeZ, 1e-6);
        const centerX = 0.5 * (mesh.boundsMin[0] + mesh.boundsMax[0]);
        const centerY = 0.5 * (mesh.boundsMin[1] + mesh.boundsMax[1]);
        const centerZ = 0.5 * (mesh.boundsMin[2] + mesh.boundsMax[2]);
        const scale = 1.7 / maxSize;

        modelMatrix.set(mat4.identity());
        mat4.scale(modelMatrix, [scale, scale, scale], modelMatrix);
        mat4.translate(modelMatrix, [-centerX, -centerY, -centerZ], modelMatrix);
        updateMatrices();

        rendererState.bindGroup = device.createBindGroup({
            layout: bindGroupLayout,
            entries: [
                { binding: 0, resource: { buffer: modelBuffer } },
                { binding: 1, resource: { buffer: viewProjectionBuffer } },
                { binding: 2, resource: { buffer: rangeBuffer } },
                { binding: 3, resource: colormapTexture.createView() },
                { binding: 4, resource: colormapSampler },
            ],
        });

        requestRender();
    }

    function renderFrame() {
        rendererState.renderScheduled = false;
        rendererState.frameCount += 1;
        const now = performance.now();

        // TODO: javítani az FPS számlálást
        if (now - rendererState.lastFpsTime >= 1000) {
            ui.fps.textContent = String(rendererState.frameCount);
            rendererState.frameCount = 0;
            rendererState.lastFpsTime = now;
        }

        const encoder = device.createCommandEncoder();
        const targetView = context.getCurrentTexture().createView();
        const pass = encoder.beginRenderPass({
            colorAttachments: [{
                view: sampleCount > 1 ? msaaTexture.createView() : targetView,
                resolveTarget: sampleCount > 1 ? targetView : undefined,
                loadOp: 'clear',
                storeOp: 'store',
                clearValue: { r: 0.03, g: 0.06, b: 0.09, a: 1 },
            }],
            depthStencilAttachment: {
                view: depthTexture.createView(),
                depthClearValue: 1,
                depthLoadOp: 'clear',
                depthStoreOp: 'store',
            },
        });

        if (rendererState.bindGroup && rendererState.drawVertexCount > 0) {
            pass.setPipeline(pipeline);
            pass.setBindGroup(0, rendererState.bindGroup);
            pass.setVertexBuffer(0, rendererState.positionBuffer);
            pass.setVertexBuffer(1, rendererState.scalarBuffer);
            pass.setVertexBuffer(2, rendererState.normalBuffer);
            pass.draw(rendererState.drawVertexCount);
        }

        pass.end();
        device.queue.submit([encoder.finish()]);
    }

    function requestRender() {
        if (rendererState.renderScheduled) return;
        rendererState.renderScheduled = true;
        requestAnimationFrame(renderFrame);
    }

    ui.canvas.addEventListener('pointerdown', (event) => {
        if (event.button !== 0) return;
        rendererState.dragging = true;
        rendererState.lastPointerX = event.clientX;
        rendererState.lastPointerY = event.clientY;
        ui.canvas.setPointerCapture(event.pointerId);
    });

    ui.canvas.addEventListener('pointerup', () => {
        rendererState.dragging = false;
    });

    ui.canvas.addEventListener('pointerleave', () => {
        rendererState.dragging = false;
    });

    ui.canvas.addEventListener('pointermove', (event) => {
        if (!rendererState.dragging) return;

        const dx = event.clientX - rendererState.lastPointerX;
        const dy = event.clientY - rendererState.lastPointerY;
        rendererState.lastPointerX = event.clientX;
        rendererState.lastPointerY = event.clientY;

        camera.rotationX -= dx * 0.004;
        camera.rotationY += dy * 0.004;
        camera.rotationY = Math.max(-1.45, Math.min(1.45, camera.rotationY));
        updateMatrices();
        requestRender();
    });

    ui.canvas.addEventListener('wheel', (event) => {
        event.preventDefault();
        camera.radius += event.deltaY * 0.002 * camera.radius;
        camera.radius = Math.max(camera.minRadius, Math.min(camera.maxRadius, camera.radius));
        updateMatrices();
        requestRender();
    }, { passive: false });

    new ResizeObserver(resize).observe(ui.canvas);
    resize();
    requestRender();

    return {
        setMesh,
        requestRender,
    };
}

function connectWebSocket(renderer) {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const socket = new WebSocket(`${protocol}//${window.location.host}/ws`);
    socket.binaryType = 'arraybuffer';
    state.socket = socket;

    socket.addEventListener('open', () => {
        state.socketReady = true;
        setStatus('Websocket connected. Load an Ensight Gold case to begin.', 'success');
        updateControlState();
    });

    socket.addEventListener('close', () => {
        state.socketReady = false;
        setStatus('Websocket disconnected. Reload the page or restart the server.', 'error');
        updateControlState();
    });

    socket.addEventListener('message', (event) => {
        try {
            if (typeof event.data === 'string') {
                const message = JSON.parse(event.data);

                if (message.type === 'welcome') {
                    return;
                }

                if (message.type === 'status') {
                    setStatus(message.message, 'info');
                    return;
                }

                if (message.type === 'error') {
                    setStatus(message.message, 'error');
                    return;
                }

                if (message.type === 'case_info') {
                    state.caseInfo = message;
                    state.currentMode = 'idle';
                    populateVariables(message);
                    ui.timeSlider.value = '0';
                    updateControlState();

                    ui.boundsMin.textContent = formatVec3(message.aabbMin);
                    ui.boundsMax.textContent = formatVec3(message.aabbMax);
                    ui.scalarRange.textContent = '-';
                    ui.mode.textContent = 'idle';
                    ui.triangles.textContent = '0';
                    ui.vertices.textContent = '0';

                    setStatus('Case loaded. Choose Load Surface or Apply Slice.', 'success');
                }

                return;
            }

            const mesh = parseMeshPacket(event.data);
            renderer.setMesh(mesh);
            updateDatasetInfo(mesh);
            setStatus(`${mesh.kind} payload received over websocket.`, 'success');
        } catch (error) {
            console.error(error);
            setStatus(error.message || 'Failed to process websocket message.', 'error');
        }
    });
}

try {
    const renderer = await createRenderer();
    connectWebSocket(renderer);
} catch (error) {
    console.error(error);
    setStatus(error.message || 'Renderer initialization failed.', 'error');
}

updateControlState();
updateTimeLabel();

ui.loadCaseBtn.addEventListener('click', () => {
    const casePath = ui.casePath.value.trim();
    if (!casePath) {
        setStatus('Enter a case path first.', 'error');
        return;
    }

    state.caseInfo = null;
    ui.boundsMin.textContent = '-';
    ui.boundsMax.textContent = '-';
    ui.scalarRange.textContent = '-';
    ui.triangles.textContent = '0';
    ui.vertices.textContent = '0';
    ui.mode.textContent = 'idle';
    updateControlState();
    sendJsonMessage({
        type: 'open_case',
        casePath,
    });
});

ui.surfaceBtn.addEventListener('click', () => {
    requestSurface();
});

ui.sliceBtn.addEventListener('click', () => {
    requestSlice();
});

ui.variableSelect.addEventListener('change', () => {
    updateControlState();
    requestCurrentView();
});

ui.timeSlider.addEventListener('input', () => {
    updateTimeLabel();
});

ui.timeSlider.addEventListener('change', () => {
    requestCurrentView();
});

ui.sliceInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
        requestSlice();
    }
});
