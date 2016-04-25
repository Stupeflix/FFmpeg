/*
 * Android MediaCodec public API functions
 *
 * Copyright (c) 2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include "libavutil/error.h"

#include "mediacodec.h"

#if CONFIG_MEDIACODEC

#include <jni.h>
#include <pthread.h>
#include <time.h>

#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"

#include "ffjni.h"
#include "mediacodecdec_common.h"
#include "version.h"

AVMediaCodecContext *av_mediacodec_alloc_context(void)
{
    return av_mallocz(sizeof(AVMediaCodecContext));
}

int av_mediacodec_default_init(AVCodecContext *avctx, AVMediaCodecContext *ctx, void *surface)
{
    int ret = 0;
    JNIEnv *env = NULL;

    env = ff_jni_get_env(avctx);
    if (!env) {
        return AVERROR_EXTERNAL;
    }

    ctx->surface = (*env)->NewGlobalRef(env, surface);
    if (ctx->surface) {
        avctx->hwaccel_context = ctx;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Could not create new global reference\n");
        ret = AVERROR_EXTERNAL;
    }

    return ret;
}

void av_mediacodec_default_free(AVCodecContext *avctx)
{
    JNIEnv *env = NULL;

    AVMediaCodecContext *ctx = avctx->hwaccel_context;

    if (!ctx) {
        return;
    }

    env = ff_jni_get_env(avctx);
    if (!env) {
        return;
    }

    if (ctx->surface) {
        (*env)->DeleteGlobalRef(env, ctx->surface);
        ctx->surface = NULL;
    }

    av_freep(&avctx->hwaccel_context);
}

int av_mediacodec_release_buffer(AVMediaCodecBuffer *buffer, int render)
{
    MediaCodecDecContext *ctx = buffer->ctx;
    int released = atomic_fetch_add(&buffer->released, 1);

    if (!released) {
        return ff_AMediaCodec_releaseOutputBuffer(ctx->codec, buffer->index, render);
    }

    return 0;
}

struct JNIAndroidSurfaceFields {

    jclass surface_class;
    jmethodID surface_init_id;
    jmethodID surface_release_id;

    jclass surface_texture_class;
    jmethodID surface_texture_init_id;
    jmethodID surface_texture_init2_id;
    jmethodID attach_to_gl_context_id;
    jmethodID detach_from_gl_context_id;
    jmethodID update_tex_image_id;
    jmethodID set_on_frame_available_listener_id;
    jmethodID set_on_frame_available_listener2_id;
    jmethodID get_transform_matrix_id;
    jmethodID set_default_buffer_size_id;
    jmethodID surface_texture_release_id;

} JNIAndroidSurfaceFields;

static const struct FFJniField jfields_mapping[] = {
    { "android/view/Surface", NULL, NULL, FF_JNI_CLASS, offsetof(struct JNIAndroidSurfaceFields, surface_class), 1 },
        { "android/view/Surface", "<init>", "(Landroid/graphics/SurfaceTexture;)V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, surface_init_id), 1 },
        { "android/view/Surface", "release", "()V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, surface_release_id), 1 },

    { "android/graphics/SurfaceTexture", NULL, NULL, FF_JNI_CLASS, offsetof(struct JNIAndroidSurfaceFields, surface_texture_class), 1 },
        { "android/graphics/SurfaceTexture", "<init>", "(I)V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, surface_texture_init_id), 1 },
        { "android/graphics/SurfaceTexture", "<init>", "(IZ)V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, surface_texture_init2_id), 0 },
        { "android/graphics/SurfaceTexture", "attachToGLContext", "(I)V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, attach_to_gl_context_id), 1 },
        { "android/graphics/SurfaceTexture", "detachFromGLContext", "()V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, detach_from_gl_context_id), 1 },
        { "android/graphics/SurfaceTexture", "updateTexImage", "()V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, update_tex_image_id), 1 },
        { "android/graphics/SurfaceTexture", "getTransformMatrix", "([F)V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, get_transform_matrix_id), 1 },
        { "android/graphics/SurfaceTexture", "setDefaultBufferSize", "(II)V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, set_default_buffer_size_id), 1 },
        { "android/graphics/SurfaceTexture", "setOnFrameAvailableListener", "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;)V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, set_on_frame_available_listener_id), 1 },
        { "android/graphics/SurfaceTexture", "setOnFrameAvailableListener", "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;Landroid/os/Handler;)V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, set_on_frame_available_listener2_id), 0 },
        { "android/graphics/SurfaceTexture", "release", "()V", FF_JNI_METHOD, offsetof(struct JNIAndroidSurfaceFields, surface_texture_release_id), 1 },


    { NULL }
};

typedef struct AndroidSurface {
    const AVClass *class;
    struct JNIAndroidSurfaceFields jfields;
    jobject *surface;
    jobject *surface_texture;
    jobject *listener;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int tex_id;
    int on_frame_available;
} AndroidSurface;

static const AVClass android_surface_class = {
    .class_name = "android_surface",
    .item_name  = av_default_item_name,
    .version    = LIBAVCODEC_VERSION_INT,
};

AVAndroidSurface *av_android_surface_new(void *listener, int tex_id)
{
    JNIEnv *env = NULL;
    jobject *surface = NULL;
    jobject *surface_texture = NULL;
    jclass listener_class = NULL;
    jmethodID listener_set_native_ptr_id = NULL;

    AVAndroidSurface *ret = NULL;

    ret = av_mallocz(sizeof(AVAndroidSurface));
    if (!ret) {
        return NULL;
    }
    ret->class = &android_surface_class;

    pthread_mutex_init(&ret->lock, NULL);
    pthread_cond_init(&ret->cond, NULL);

    env = ff_jni_get_env(ret);
    if (!env) {
        av_freep(&ret);
        return NULL;
    }

    if (ff_jni_init_jfields(env, &ret->jfields, jfields_mapping, 1, NULL) < 0) {
        goto fail;
    }

    surface_texture = (*env)->NewObject(env,
                                        ret->jfields.surface_texture_class,
                                        ret->jfields.surface_texture_init_id,
                                        tex_id);
    if (!surface_texture) {
        goto fail;
    }

    ret->surface_texture = (*env)->NewGlobalRef(env, surface_texture);
    if (!ret->surface_texture) {
        goto fail;
    }
    ret->tex_id = tex_id;

    surface = (*env)->NewObject(env,
                                ret->jfields.surface_class,
                                ret->jfields.surface_init_id,
                                ret->surface_texture);
    if (!surface) {
        goto fail;
    }

    ret->surface = (*env)->NewGlobalRef(env, surface);
    if (!ret->surface) {
        goto fail;
    }

    if (listener) {
        ret->listener = (*env)->NewGlobalRef(env, listener);
        if (!ret->listener) {
            goto fail;
        }

        (*env)->CallVoidMethod(env,
                               ret->surface_texture,
                               ret->jfields.set_on_frame_available_listener_id,
                               listener);
        if (ff_jni_exception_check(env, 1, ret) < 0) {
            goto fail;
        }

        listener_class = (*env)->GetObjectClass(env, listener);
        if (ff_jni_exception_check(env, 1, ret) < 0) {
            goto fail;
        }

        listener_set_native_ptr_id = (*env)->GetMethodID(env,
                                                        listener_class,
                                                        "setNativePtr",
                                                        "(J)V");
        if (ff_jni_exception_check(env, 1, ret) < 0) {
            goto fail;
        }

        (*env)->CallVoidMethod(env, listener, listener_set_native_ptr_id, (jlong)ret);
        if (ff_jni_exception_check(env, 1, ret) < 0) {
            goto fail;
        }
    }

    if (surface) {
        (*env)->DeleteLocalRef(env, surface);
    }
    if (surface_texture) {
        (*env)->DeleteLocalRef(env, surface_texture);
    }
    if (listener_class) {
        (*env)->DeleteLocalRef(env, listener_class);
    }

    return ret;
fail:
    if (surface) {
        (*env)->DeleteLocalRef(env, surface);
    }

    if (surface_texture) {
        (*env)->DeleteLocalRef(env, surface_texture);
    }

    if (listener_class) {
        (*env)->DeleteLocalRef(env, listener_class);
    }

    av_android_surface_free(&ret);

    return NULL;
}

void av_android_surface_free(AVAndroidSurface **surface)
{
    JNIEnv *env = NULL;

    if (!surface || !*surface) {
        return;
    }

    env = ff_jni_get_env(*surface);
    if (!env) {
        av_freep(surface);
        return;
    }

    if ((*surface)->surface) {
        (*env)->CallVoidMethod(env, (*surface)->surface, (*surface)->jfields.surface_release_id);
        if (ff_jni_exception_check(env, 1, surface) < 0) {
            goto fail;
        }
    }

    if ((*surface)->surface_texture) {
        (*env)->CallVoidMethod(env, (*surface)->surface_texture, (*surface)->jfields.surface_texture_release_id);
        if (ff_jni_exception_check(env, 1, surface) < 0) {
            goto fail;
        }
    }

fail:
    if ((*surface)->surface) {
        (*env)->DeleteGlobalRef(env, (*surface)->surface);
        (*surface)->surface = NULL;
    }

    if ((*surface)->surface_texture) {
        (*env)->DeleteGlobalRef(env, (*surface)->surface_texture);
        (*surface)->surface_texture = NULL;
    }

    if ((*surface)->listener) {
        (*env)->DeleteGlobalRef(env, (*surface)->listener);
        (*surface)->listener = NULL;
    }

    ff_jni_reset_jfields(env, &(*surface)->jfields, jfields_mapping, 1, *surface);

    pthread_mutex_destroy(&(*surface)->lock);
    pthread_cond_destroy(&(*surface)->cond);
    av_freep(surface);
}


void *av_android_surface_get_surface(AVAndroidSurface *surface)
{
    if (!surface) {
        return NULL;
    }

    return surface->surface;
}


int av_android_surface_attach_to_gl_context(AVAndroidSurface *surface, int tex_id)
{
    int ret = 0;
    JNIEnv *env = NULL;

    if (!surface) {
        return 0;
    }

    env = ff_jni_get_env(surface);
    if (!env) {
        av_freep(surface);
        return AVERROR_EXTERNAL;
    }

    if (surface->tex_id != tex_id) {
        av_android_surface_detach_from_gl_context(surface);
    }

    (*env)->CallVoidMethod(env,
                           surface->surface_texture,
                           surface->jfields.attach_to_gl_context_id,
                           tex_id);
    if (ff_jni_exception_check(env, 1, surface) < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    surface->tex_id = tex_id;

fail:
    return ret;
}

int av_android_surface_detach_from_gl_context(AVAndroidSurface *surface)
{
    int ret = 0;
    JNIEnv *env = NULL;

    if (!surface || surface->tex_id < 0) {
        return 0;
    }

    env = ff_jni_get_env(surface);
    if (!env) {
        av_freep(surface);
        return AVERROR_EXTERNAL;
    }

    (*env)->CallVoidMethod(env,
                           surface->surface_texture,
                           surface->jfields.detach_from_gl_context_id);
    if (ff_jni_exception_check(env, 1, surface) < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    surface->tex_id = -1;

fail:
    return ret;
}

int av_android_surface_render_buffer(AVAndroidSurface *surface, AVMediaCodecBuffer *buffer, float *matrix)
{
    int ret = 0;
    JNIEnv *env = NULL;
    jfloatArray array = NULL;

    if (!surface) {
        return 0;
    }

    env = ff_jni_get_env(surface);
    if (!env) {
        av_freep(surface);
        return AVERROR_EXTERNAL;
    }

    pthread_mutex_lock(&surface->lock);
    surface->on_frame_available = 0;

    if (av_mediacodec_release_buffer(buffer, 1) < 0) {
        pthread_mutex_unlock(&surface->lock);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (surface->listener && !surface->on_frame_available) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 30000000;
        pthread_cond_timedwait(&surface->cond, &surface->lock, &ts);
    }

    ret = surface->on_frame_available;
    pthread_mutex_unlock(&surface->lock);

    if (!ret) {
        av_log(surface, AV_LOG_WARNING, "No frame available\n");
    }

    (*env)->CallVoidMethod(env, surface->surface_texture, surface->jfields.update_tex_image_id);
    if (ff_jni_exception_check(env, 1, surface) < 0) {
        pthread_mutex_unlock(&surface->lock);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    array = (*env)->NewFloatArray(env, 16);
    if (!array) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    (*env)->CallVoidMethod(env,
                           surface->surface_texture,
                           surface->jfields.get_transform_matrix_id,
                           array);
    if (ff_jni_exception_check(env, 1, surface) < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    (*env)->GetFloatArrayRegion(env, array, 0, 16, matrix);
    if (ff_jni_exception_check(env, 1, surface) < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

fail:
    if (array) {
        (*env)->DeleteLocalRef(env, array);
    }

    return ret;
}

void av_android_surface_signal_frame(AVAndroidSurface *surface)
{
    if (!surface) {
        return;
    }

    pthread_mutex_lock(&surface->lock);
    surface->on_frame_available = 1;
    pthread_cond_signal(&surface->cond);
    pthread_mutex_unlock(&surface->lock);
}

struct JNIAndroidLooperFields {

    jclass looper_class;
    jmethodID prepare_id;
    jmethodID my_looper_id;
    jmethodID get_main_looper_id;
    jmethodID loop_id;
    jmethodID quit_id;

} JNIAndroidLooperFields;

static const struct FFJniField android_looper_mapping[] = {
    { "android/os/Looper", NULL, NULL, FF_JNI_CLASS, offsetof(struct JNIAndroidLooperFields, looper_class), 1 },
        { "android/os/Looper", "prepare", "()V", FF_JNI_STATIC_METHOD, offsetof(struct JNIAndroidLooperFields, prepare_id), 1 },
        { "android/os/Looper", "myLooper", "()Landroid/os/Looper;", FF_JNI_STATIC_METHOD, offsetof(struct JNIAndroidLooperFields, my_looper_id), 1 },
        { "android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", FF_JNI_STATIC_METHOD, offsetof(struct JNIAndroidLooperFields, get_main_looper_id), 1 },
        { "android/os/Looper", "loop", "()V", FF_JNI_STATIC_METHOD, offsetof(struct JNIAndroidLooperFields, loop_id), 1 },
        { "android/os/Looper", "quit", "()V", FF_JNI_METHOD, offsetof(struct JNIAndroidLooperFields, quit_id), 1 },

    { NULL }
};

typedef struct AndroidLooper {
    const AVClass *class;
    struct JNIAndroidLooperFields jfields;
    jobject *looper;
} AndroidLooper;

static const AVClass android_looper_class = {
    .class_name = "android_looper",
    .item_name  = av_default_item_name,
    .version    = LIBAVCODEC_VERSION_INT,
};

AVAndroidLooper *av_android_looper_new(void)
{
    JNIEnv *env = NULL;
    AVAndroidLooper *ret = NULL;

    ret = av_mallocz(sizeof(AVAndroidLooper));
    if (!ret) {
        return NULL;
    }
    ret->class = &android_looper_class;

    env = ff_jni_get_env(ret);
    if (!env) {
        av_freep(&ret);
        return NULL;
    }

    if (ff_jni_init_jfields(env, &ret->jfields, android_looper_mapping, 1, NULL) < 0) {
        goto fail;
    }

    return ret;
fail:
    av_android_looper_free(&ret);

    return NULL;
}

int av_android_looper_prepare(AVAndroidLooper *looper)
{
    int ret = 0;
    JNIEnv *env = NULL;
    jobject *my_looper = NULL;
    jobject *main_looper = NULL;

    if (!looper) {
        return 0;
    }

    env = ff_jni_get_env(looper);
    if (!env) {
        return AVERROR_EXTERNAL;
    }

    (*env)->CallStaticVoidMethod(env,
                                 looper->jfields.looper_class,
                                 looper->jfields.prepare_id);
    if (ff_jni_exception_check(env, 1, looper) < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    my_looper = (*env)->CallStaticObjectMethod(env,
                                               looper->jfields.looper_class,
                                               looper->jfields.my_looper_id);
    if (ff_jni_exception_check(env, 1, looper) < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    main_looper = (*env)->CallStaticObjectMethod(env,
                                                 looper->jfields.looper_class,
                                                 looper->jfields.get_main_looper_id);
    if (ff_jni_exception_check(env, 1, looper) < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    looper->looper = (*env)->NewGlobalRef(env, my_looper);
    if (!looper->looper) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

fail:
    if (my_looper) {
        (*env)->DeleteLocalRef(env, my_looper);
        my_looper = NULL;
    }

    if (main_looper) {
        (*env)->DeleteLocalRef(env, main_looper);
        main_looper = NULL;
    }

    return ret;
}

int av_android_looper_loop(AVAndroidLooper *looper)
{
    int ret = 0;
    JNIEnv *env = NULL;

    if (!looper) {
        return 0;
    }

    env = ff_jni_get_env(looper);
    if (!env) {
        return AVERROR_EXTERNAL;
    }

    (*env)->CallStaticVoidMethod(env,
                                 looper->jfields.looper_class,
                                 looper->jfields.loop_id);
    if (ff_jni_exception_check(env, 1, looper) < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

fail:
    return ret;
}

int av_android_looper_quit(AVAndroidLooper *looper)
{
    int ret = 0;
    JNIEnv *env = NULL;

    if (!looper) {
        return 0;
    }

    env = ff_jni_get_env(looper);
    if (!env) {
        return AVERROR_EXTERNAL;
    }

    (*env)->CallVoidMethod(env,
                           looper->looper,
                           looper->jfields.quit_id);
    if (ff_jni_exception_check(env, 1, looper) < 0) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

fail:
    return ret;
}

void av_android_looper_free(AVAndroidLooper **looper)
{
    JNIEnv *env = NULL;

    if (!looper || !*looper) {
        return;
    }

    env = ff_jni_get_env(*looper);
    if (!env) {
        av_freep(looper);
        return;
    }
    if ((*looper)->looper) {
        (*env)->DeleteGlobalRef(env, (*looper)->looper);
        (*looper)->looper = NULL;
    }

    ff_jni_reset_jfields(env, &(*looper)->jfields, android_looper_mapping, 1, *looper);

    av_freep(looper);
}

#else

#include <stdlib.h>

AVMediaCodecContext *av_mediacodec_alloc_context(void)
{
    return NULL;
}

int av_mediacodec_default_init(AVCodecContext *avctx, AVMediaCodecContext *ctx, void *surface)
{
    return AVERROR(ENOSYS);
}

void av_mediacodec_default_free(AVCodecContext *avctx)
{
}

int av_mediacodec_release_buffer(AVMediaCodecBuffer *buffer, int render)
{
    return AVERROR(ENOSYS);
}

AVAndroidSurface *av_android_surface_new(void* listener, int tex_id)
{
    return NULL;
}

void av_android_surface_free(AVAndroidSurface **surface)
{
}

void *av_android_surface_get_surface(AVAndroidSurface *surface)
{
    return NULL;
}

int av_android_surface_attach_to_gl_context(AVAndroidSurface *surface, int tex_id)
{
    return 0;
}

int av_android_surface_detach_from_gl_context(AVAndroidSurface *surface)
{
    return 0;
}

int av_android_surface_render_buffer(AVAndroidSurface *surface, AVMediaCodecBuffer *buffer, float *matrix)
{
    return 0;
}

void av_android_surface_signal_frame(AVAndroidSurface *surface)
{
}

AVAndroidLooper *av_android_looper_new()
{
    return NULL;
}

int av_android_looper_prepare(AVAndroidLooper *looper)
{
    return 0;
}

int av_android_looper_loop(AVAndroidLooper *looper)
{
    return 0;
}

int av_android_looper_quit(AVAndroidLooper *looper)
{
    return 0;
}

void av_android_looper_free(AVAndroidLooper **looper)
{
}

#endif
