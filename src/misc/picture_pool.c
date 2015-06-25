/*****************************************************************************
 * picture_pool.c : picture pool functions
 *****************************************************************************
 * Copyright (C) 2009 VLC authors and VideoLAN
 * Copyright (C) 2009 Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 * Copyright (C) 2013-2015 Rémi Denis-Courmont
 *
 * Authors: Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <assert.h>
#include <limits.h>
#include <stdlib.h>

#include <vlc_common.h>
#include <vlc_picture_pool.h>
#include "picture.h"

/*****************************************************************************
 *
 *****************************************************************************/
struct picture_gc_sys_t {
    picture_pool_t *pool;
    picture_t *picture;
    unsigned offset;
};

struct picture_pool_t {
    int       (*pic_lock)(picture_t *);
    void      (*pic_unlock)(picture_t *);
    vlc_mutex_t lock;

    unsigned long long available;
    unsigned    refs;
    unsigned    picture_count;
    picture_t  *picture[];
};

void picture_pool_Release(picture_pool_t *pool)
{
    bool destroy;

    vlc_mutex_lock(&pool->lock);
    assert(pool->refs > 0);
    destroy = --pool->refs == 0;
    vlc_mutex_unlock(&pool->lock);

    if (likely(!destroy))
        return;

    for (unsigned i = 0; i < pool->picture_count; i++) {
        picture_t *picture = pool->picture[i];
        picture_priv_t *priv = (picture_priv_t *)picture;

        picture_Release(priv->gc.opaque->picture);
        free(priv->gc.opaque);
        free(picture);
    }

    vlc_mutex_destroy(&pool->lock);
    free(pool);
}

static void picture_pool_ReleasePicture(picture_t *picture)
{
    picture_priv_t *priv = (picture_priv_t *)picture;
    picture_gc_sys_t *sys = priv->gc.opaque;
    picture_pool_t *pool = sys->pool;

    if (pool->pic_unlock != NULL)
        pool->pic_unlock(picture);

    vlc_mutex_lock(&pool->lock);
    assert(!(pool->available & (1ULL << sys->offset)));
    pool->available |= 1ULL << sys->offset;
    vlc_mutex_unlock(&pool->lock);

    picture_pool_Release(pool);
}

static picture_t *picture_pool_ClonePicture(picture_pool_t *pool,
                                            picture_t *picture,
                                            unsigned offset)
{
    picture_gc_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return NULL;

    sys->pool = pool;
    sys->picture = picture;
    sys->offset = offset;

    picture_resource_t res = {
        .p_sys = picture->p_sys,
        .pf_destroy = picture_pool_ReleasePicture,
    };

    for (int i = 0; i < picture->i_planes; i++) {
        res.p[i].p_pixels = picture->p[i].p_pixels;
        res.p[i].i_lines = picture->p[i].i_lines;
        res.p[i].i_pitch = picture->p[i].i_pitch;
    }

    picture_t *clone = picture_NewFromResource(&picture->format, &res);
    if (likely(clone != NULL))
        ((picture_priv_t *)clone)->gc.opaque = sys;
    else
        free(sys);

    return clone;
}

picture_pool_t *picture_pool_NewExtended(const picture_pool_configuration_t *cfg)
{
    if (unlikely(cfg->picture_count > CHAR_BIT * sizeof (unsigned long long)))
        return NULL;

    picture_pool_t *pool = malloc(sizeof (*pool)
                                  + cfg->picture_count * sizeof (picture_t *));
    if (unlikely(pool == NULL))
        return NULL;

    pool->pic_lock   = cfg->lock;
    pool->pic_unlock = cfg->unlock;
    vlc_mutex_init(&pool->lock);
    pool->available = (1ULL << cfg->picture_count) - 1;
    pool->refs = 1;
    pool->picture_count = cfg->picture_count;

    for (unsigned i = 0; i < cfg->picture_count; i++) {
        picture_t *picture = picture_pool_ClonePicture(pool, cfg->picture[i], i);
        if (unlikely(picture == NULL))
            abort();

        atomic_init(&((picture_priv_t *)picture)->gc.refs, 0);

        pool->picture[i] = picture;
    }
    return pool;

}

picture_pool_t *picture_pool_New(unsigned count, picture_t *const *tab)
{
    picture_pool_configuration_t cfg = {
        .picture_count = count,
        .picture = tab,
    };

    return picture_pool_NewExtended(&cfg);
}

picture_pool_t *picture_pool_NewFromFormat(const video_format_t *fmt,
                                           unsigned count)
{
    picture_t *picture[count ? count : 1];
    unsigned i;

    for (i = 0; i < count; i++) {
        picture[i] = picture_NewFromFormat(fmt);
        if (picture[i] == NULL)
            goto error;
    }

    picture_pool_t *pool = picture_pool_New(count, picture);
    if (!pool)
        goto error;

    return pool;

error:
    while (i > 0)
        picture_Release(picture[--i]);
    return NULL;
}

picture_pool_t *picture_pool_Reserve(picture_pool_t *master, unsigned count)
{
    picture_t *picture[count ? count : 1];
    unsigned i;

    for (i = 0; i < count; i++) {
        picture[i] = picture_pool_Get(master);
        if (picture[i] == NULL)
            goto error;
    }

    picture_pool_t *pool = picture_pool_New(count, picture);
    if (!pool)
        goto error;

    return pool;

error:
    while (i > 0)
        picture_Release(picture[--i]);
    return NULL;
}

picture_t *picture_pool_Get(picture_pool_t *pool)
{
    vlc_mutex_lock(&pool->lock);
    assert(pool->refs > 0);

    for (unsigned i = 0; i < pool->picture_count; i++) {
        picture_t *picture = pool->picture[i];

        if (!(pool->available & (1ULL << i)))
            continue;

        pool->available &= ~(1ULL << i);
        pool->refs++;
        vlc_mutex_unlock(&pool->lock);

        if (pool->pic_lock != NULL && pool->pic_lock(picture) != 0) {
            vlc_mutex_lock(&pool->lock);
            pool->available |= 1ULL << i;
            pool->refs--;
            continue;
        }

        assert(atomic_load(&((picture_priv_t *)picture)->gc.refs) == 0);
        atomic_init(&((picture_priv_t *)picture)->gc.refs, 1);
        picture->p_next = NULL;
        return picture;
    }

    vlc_mutex_unlock(&pool->lock);
    return NULL;
}

unsigned picture_pool_Reset(picture_pool_t *pool)
{
    unsigned ret = 0;
retry:
    vlc_mutex_lock(&pool->lock);
    assert(pool->refs > 0);

    for (unsigned i = 0; i < pool->picture_count; i++) {
        picture_t *picture = pool->picture[i];

        if (!(pool->available & (1ULL << i))) {
            vlc_mutex_unlock(&pool->lock);
            picture_Release(picture);
            ret++;
            goto retry;
        }
    }
    vlc_mutex_unlock(&pool->lock);

    return ret;
}

unsigned picture_pool_GetSize(const picture_pool_t *pool)
{
    return pool->picture_count;
}

void picture_pool_Enum(picture_pool_t *pool, void (*cb)(void *, picture_t *),
                       void *opaque)
{
    /* NOTE: So far, the pictures table cannot change after the pool is created
     * so there is no need to lock the pool mutex here. */
    for (unsigned i = 0; i < pool->picture_count; i++)
        cb(opaque, pool->picture[i]);
}
