/*
 *  Copyright (C) 2016 Cisco and/or its affiliates. All rights reserved.
 *
 *  Author: Kevin Lin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL.  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so.  If you
 *  do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#ifdef	HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef	HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <zlib.h>

#if HAVE_ICONV
#include <iconv.h>
#endif

#include "clamav.h"
#include "others.h"
#include "pdf.h"
#include "pdfdecode.h"
#include "str.h"
#include "bytecode.h"
#include "bytecode_api.h"

struct pdf_token {
    uint32_t length;
    uint8_t *content;
};

static  int filter_ascii85decode(struct pdf_token *token);
static  int filter_rldecode(struct pdf_token *token);
static  int filter_flatedecode(struct pdf_struct *pdf, struct pdf_obj *obj, struct pdf_dict *params, struct pdf_token *token);
static  int filter_asciihexdecode(struct pdf_token *token);
static  int filter_decrypt(struct pdf_struct *pdf, struct pdf_obj *obj, struct pdf_dict *params, struct pdf_token *token, int mode);

static  int pdf_decodestream_internal(struct pdf_struct *pdf, struct pdf_obj *obj, struct pdf_dict *params, struct pdf_token *token);

int pdf_decodestream(struct pdf_struct *pdf, struct pdf_obj *obj, struct pdf_dict *params, const char *stream, uint32_t streamlen, int fout)
{
    struct pdf_token *token;
    int rc;

    if (!stream || fout < 0 || !obj->numfilters) {
        cli_dbgmsg("nothing to decode\n");
        return CL_ENULLARG;
    }

#if 0
    if (params)
        pdf_print_dict(params, 0);
#endif

    token = cli_malloc(sizeof(struct pdf_token));
    if (!token)
        return CL_EMEM;

    token->content = cli_malloc(streamlen);
    if (!token->content) {
        free(token);
        return CL_EMEM;
    }
    memcpy(token->content, stream, streamlen);
    token->length = streamlen;

    rc = pdf_decodestream_internal(pdf, obj, params, token);

    if ((rc == CL_SUCCESS) && !cli_checklimits("pdf", pdf->ctx, token->length, 0, 0)) {
        if (cli_writen(fout, token->content, token->length) != token->length) {
            cli_errmsg("cli_pdf: failed to write output file\n");
            rc = CL_EWRITE;
        }
    }

    free(token->content);
    free(token);
    return rc;
}

static int pdf_decodestream_internal(struct pdf_struct *pdf, struct pdf_obj *obj, struct pdf_dict *params, struct pdf_token *token)
{
    const char *filter = NULL;
    int i, rc;

    cli_dbgmsg("cli_pdf: detected %lu filters applied\n", (long unsigned)(obj->numfilters));

    /*
     * if pdf is decryptable, scan for CRYPT filter
     * if none, force a DECRYPT filter application
     */
    if (!(obj->flags & (1 << OBJ_FILTER_CRYPT))) {
        cli_dbgmsg("cli_pdf: decoding => non-filter CRYPT\n");
        if ((rc = filter_decrypt(pdf, obj, params, token, 1)) != CL_SUCCESS)
            return rc;

    }

    /* TODO - MAY BE SUBJECT TO CHANGE */
    for (i = 0; i < obj->numfilters; i++) {
        switch(obj->filterlist[i]) {
        case OBJ_FILTER_A85:
            cli_dbgmsg("cli_pdf: decoding [%d] => ASCII85DECODE\n", obj->filterlist[i]);
            rc = filter_ascii85decode(token);
            break;

        case OBJ_FILTER_RL:
            cli_dbgmsg("cli_pdf: decoding [%d] => RLDECODE\n", obj->filterlist[i]);
            rc = filter_rldecode(token);
            break;

        case OBJ_FILTER_FLATE:
            cli_dbgmsg("cli_pdf: decoding [%d] => FLATEDECODE\n", obj->filterlist[i]);
            rc = filter_flatedecode(pdf, obj, params, token);
            break;

        case OBJ_FILTER_AH:
            cli_dbgmsg("cli_pdf: decoding [%d] => ASCIIHEXDECODE\n", obj->filterlist[i]);
            rc = filter_asciihexdecode(token);
            break;

        case OBJ_FILTER_CRYPT:
            cli_dbgmsg("cli_pdf: decoding [%d] => CRYPT\n", obj->filterlist[i]);
            rc = filter_decrypt(pdf, obj, params, token, 0);
            break;

        case OBJ_FILTER_JPX:
            if (!filter) filter = "JPXDECODE";
        case OBJ_FILTER_DCT:
            if (!filter) filter = "DCTDECODE";
        case OBJ_FILTER_LZW:
            if (!filter) filter = "LZWDECODE";
        case OBJ_FILTER_FAX:
            if (!filter) filter = "FAXDECODE";
        case OBJ_FILTER_JBIG2:
            if (!filter) filter = "JBIG2DECODE";

            cli_warnmsg("cli_pdf: unimplemented filter type [%d] => %s\n", obj->filterlist[i], filter);
            filter = NULL;
            rc = CL_BREAK;
            break;

        default:
            cli_warnmsg("cli_pdf: unknown filter type [%d]\n", obj->filterlist[i]);
            rc = CL_BREAK;
            break;
        }

        /*TODO: check rc value*/
        /*TODO: check content field*/
        /*TODO: check length value*/
        /*IF INTERMEDIATE DUMPING - PUT HERE*/
    }

    return CL_SUCCESS;
}

/*
 * ascii85 inflation
 * See http://www.piclist.com/techref/method/encode.htm (look for base85)
 */
static int filter_ascii85decode(struct pdf_token *token)
{
    uint8_t *decoded;
    uint32_t declen = 0;

    const uint8_t *ptr = (uint8_t *)token->content;
    uint32_t remaining = token->length;
    int quintet = 0, rc = CL_SUCCESS;
    uint64_t sum = 0;

    /* 5:4 decoding ratio - (5*length), ((length/5+1)*4), (((x+4)/5)*4) */
    if (!(decoded = (uint8_t *)cli_malloc(((remaining+4)/5)*4))) {
        cli_errmsg("cli_pdf: cannot allocate memory for decoded output\n");
        return CL_EMEM;
    }

    if(cli_memstr(ptr, remaining, "~>", 2) == NULL)
        cli_dbgmsg("cli_pdf: no EOF marker found\n");

    cli_dbgmsg("cli_pdf: %lu bytes remaining\n", (unsigned long)remaining);

    while (remaining > 0) {
        int byte = (remaining--) ? (int)*ptr++ : EOF;

        if((byte == '~') && (remaining > 0) && (*ptr == '>'))
            byte = EOF;

        if(byte >= '!' && byte <= 'u') {
            sum = (sum * 85) + ((uint32_t)byte - '!');
            if(++quintet == 5) {
                *decoded++ = (unsigned char)(sum >> 24);
                *decoded++ = (unsigned char)((sum >> 16) & 0xFF);
                *decoded++ = (unsigned char)((sum >> 8) & 0xFF);
                *decoded++ = (unsigned char)(sum & 0xFF);

                declen += 4;
                quintet = 0;
                sum = 0;
            }
        } else if(byte == 'z') {
            if(quintet) {
                cli_dbgmsg("cli_pdf: unexpected 'z'\n");
                rc = CL_EFORMAT;
                break;
            }

            *decoded++ = '\0';
            *decoded++ = '\0';
            *decoded++ = '\0';
            *decoded++ = '\0';

            declen += 4;
        } else if(byte == EOF) {
            cli_dbgmsg("cli_pdf: %d bytes remaining in last quintet\n", quintet);
            if(quintet) {
                int i;

                if(quintet == 1) {
                    cli_dbgmsg("cli_pdf: only 1 byte in last quintet\n");
                    rc = CL_EFORMAT;
                    break;
                }

                for(i = quintet; i < 5; i++)
                    sum *= 85;

                if(quintet > 1)
                    sum += (0xFFFFFF >> ((quintet - 2) * 8));

                for(i = 0; i < quintet - 1; i++)
                    *decoded++ = (uint8_t)((sum >> (24 - 8 * i)) & 0xFF);
                declen += quintet-1;
            }

            break;
        } else if(!isspace(byte)) {
            cli_dbgmsg("cli_pdf: invalid character 0x%x @ %lu\n",
                       byte & 0xFF, (unsigned long)(token->length-remaining));

            rc = CL_EFORMAT;
            break;
        }
    }

    if (rc == CL_SUCCESS) {
        free(token->content);

        cli_dbgmsg("cli_pdf: deflated %lu bytes from %lu total bytes\n",
                   (unsigned long)declen, (unsigned long)(token->length));

        token->content = decoded;
        token->length = declen;
    } else {
        cli_errmsg("cli_pdf: error occurred parsing byte %lu of %lu\n",
                   (unsigned long)(token->length-remaining), (unsigned long)(token->length));
        free(decoded);
    }
    return rc;
}

/* imported from razorback */
static int filter_rldecode(struct pdf_token *token)
{
    uint8_t *decoded, *temp;
    uint32_t declen = 0, capacity = 0;

    uint8_t *content = (uint8_t *)token->content;
    uint32_t length = token->length;
    uint32_t offset = 0;
    int rc = CL_SUCCESS;

    if (!(decoded = cli_calloc(BUFSIZ, sizeof(uint8_t)))) {
        cli_errmsg("cli_pdf: cannot allocate memory for decoded output\n");
        return CL_EMEM;
    }
    capacity = BUFSIZ;

    while (offset < length) {
        uint8_t srclen = content[offset++];
        if (srclen < 128) {
            /* direct copy of (srclen + 1) bytes */
            if (offset + srclen + 1 > length) {
                cli_dbgmsg("cli_pdf: required source length (%lu) exceeds remaining length (%lu)\n",
                           (long unsigned)(offset+srclen+1), (long unsigned)(length-offset));
                rc = CL_EFORMAT;
                break;
            }
            if (declen + srclen + 1 > capacity) {
                /* TODO - limit check */
                if (!(temp = cli_realloc(decoded, capacity + BUFSIZ))) {
                    cli_errmsg("cli_pdf: cannot reallocate memory for decoded output\n");
                    rc = CL_EMEM;
                    break;
                }
                decoded = temp;
                capacity += BUFSIZ;
            }

            memcpy(decoded+declen, content+offset, srclen+1);
            offset += srclen + 1;
            declen += srclen + 1;
        } else if (srclen > 128) {
            /* copy the next byte (257 - srclen) times */
            if (offset + 1 > length) {
                cli_dbgmsg("cli_pdf: required source length (%lu) exceeds remaining length (%lu)\n",
                           (long unsigned)(offset+srclen+1), (long unsigned)(length-offset));
                rc = CL_EFORMAT;
                break;
            }
            if (declen + (257 - srclen) + 1 > capacity) {
                /* TODO - limit check */
                if (!(temp = cli_realloc(decoded, capacity + BUFSIZ))) {
                    cli_errmsg("cli_pdf: cannot reallocate memory for decoded output\n");
                    rc = CL_EMEM;
                    break;
                }
                decoded = temp;
                capacity += BUFSIZ;
            }

            memset(decoded+declen, content[offset], 257-srclen);
            offset++;
            declen += 257 - srclen;
        } else { /* srclen == 128 */
            /* end of data */
            cli_dbgmsg("cli_pdf: end-of-stream marker @ offset %lu (%lu bytes remaining)\n",
                       (unsigned long)offset, (long unsigned)(token->length-offset));
            break;
        }
    }

    if (rc == CL_SUCCESS) {
        free(token->content);

        cli_dbgmsg("cli_pdf: inflated %lu bytes from %lu total bytes\n",
                   (unsigned long)declen, (unsigned long)(token->length));

        token->content = decoded;
        token->length = declen;
    } else {
        cli_errmsg("cli_pdf: error occurred parsing byte %lu of %lu\n",
                   (unsigned long)offset, (unsigned long)(token->length));
        free(decoded);
    }
    return rc;
}

static uint8_t *decode_nextlinestart(uint8_t *content, uint32_t length)
{
    uint8_t *pt = content;
    uint32_t r;
    int toggle = 0;

    for (r = 0; r < length; r++, pt++) {
        if (*pt == '\n' || *pt == '\r')
            toggle = 1;
        else if (toggle)
            break;
    }

    return pt;
}

static int filter_flatedecode(struct pdf_struct *pdf, struct pdf_obj *obj, struct pdf_dict *params, struct pdf_token *token)
{
    uint8_t *decoded, *temp;
    uint32_t declen = 0, capacity = 0;

    uint8_t *content = (uint8_t *)token->content;
    uint32_t length = token->length;
    z_stream stream;
    int zstat, skip = 0, rc = CL_SUCCESS;

    UNUSEDPARAM(params);

    if (length == 0)
        return CL_CLEAN;

    if (*content == '\r') {
        content++;
        length--;
        pdfobj_flag(pdf, obj, BAD_STREAMSTART);
        /* PDF spec says stream is followed by \r\n or \n, but not \r alone.
         * Sample 0015315109, it has \r followed by zlib header.
         * Flag pdf as suspicious, and attempt to extract by skipping the \r.
         */
        if (!length)
            return CL_CLEAN;
    }

    if (!(decoded = (uint8_t *)cli_calloc(BUFSIZ, sizeof(uint8_t)))) {
        cli_errmsg("cli_pdf: cannot allocate memory for decoded output\n");
        return CL_EMEM;
    }
    capacity = BUFSIZ;

    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)content;
    stream.avail_in = length;
    stream.next_out = (Bytef *)decoded;
    stream.avail_out = BUFSIZ;

    zstat = inflateInit(&stream);
    if(zstat != Z_OK) {
        cli_warnmsg("cli_pdf: inflateInit failed\n");
        free(decoded);
        return CL_EMEM;
    }

    /* initial inflate */
    zstat = inflate(&stream, Z_NO_FLUSH);
    /* check if nothing written whatsoever */
    if ((zstat != Z_OK) && (stream.avail_out == BUFSIZ)) {
        /* skip till EOL, and try inflating from there, sometimes
         * PDFs contain extra whitespace */
        uint8_t *q = decode_nextlinestart(content, length);
        if (q) {
            (void)inflateEnd(&stream);
            length -= q - content;
            content = q;

            stream.next_in = (Bytef *)content;
            stream.avail_in = length;
            stream.next_out = (Bytef *)decoded;
            stream.avail_out = capacity;

            zstat = inflateInit(&stream);
            if(zstat != Z_OK) {
                cli_warnmsg("cli_pdf: inflateInit failed\n");
                free(decoded);
                return CL_EMEM;
            }

            pdfobj_flag(pdf, obj, BAD_FLATESTART);
        }

        zstat = inflate(&stream, Z_NO_FLUSH);
    }

    while (zstat == Z_OK && stream.avail_in) {
        /* extend output capacity if needed, TODO - limit check */
        if(stream.avail_out == 0) {
            if(stream.avail_out == 0) {
                if (!(temp = cli_realloc(decoded, capacity + BUFSIZ))) {
                    cli_errmsg("cli_pdf: cannot reallocate memory for decoded output\n");
                    rc = CL_EMEM;
                    break;
                }
                decoded = temp;
                stream.next_out = decoded + capacity;
                stream.avail_out = BUFSIZ;
                declen += BUFSIZ;
                capacity += BUFSIZ;
            }
        }

        /* continue inflation */
        zstat = inflate(&stream, Z_NO_FLUSH);
    }

    /* add stream end fragment to decoded length */
    declen += (BUFSIZ - stream.avail_out);

    /* error handling */
    switch(zstat) {
    case Z_OK:
        cli_dbgmsg("cli_pdf: Z_OK on stream inflation completion\n");
        /* intentional fall-through */
    case Z_STREAM_END:
        cli_dbgmsg("cli_pdf: inflated %lu bytes from %lu total bytes (%lu bytes remaining)\n",
                   (unsigned long)declen, (unsigned long)(token->length), (unsigned long)(stream.avail_in));
        break;

    /* potentially fatal - *mostly* ignored as per older version */
    case Z_STREAM_ERROR:
    case Z_NEED_DICT:
    case Z_DATA_ERROR:
    case Z_MEM_ERROR:
    default:
        if(stream.msg)
            cli_dbgmsg("cli_pdf: after writing %lu bytes, got error \"%s\" inflating PDF stream in %u %u obj\n",
                       (unsigned long)declen, stream.msg, obj->id>>8, obj->id&0xff);
        else
            cli_dbgmsg("cli_pdf: after writing %lu bytes, got error %d inflating PDF stream in %u %u obj\n",
                       (unsigned long)declen, zstat, obj->id>>8, obj->id&0xff);

        if (declen == 0) {
            pdfobj_flag(pdf, obj, BAD_FLATESTART);
            cli_dbgmsg("cli_pdf: no bytes were inflated.\n");

            rc = CL_EFORMAT;
        } else {
            pdfobj_flag(pdf, obj, BAD_FLATE);
        }
        break;
    }

    (void)inflateEnd(&stream);

    if (rc == CL_SUCCESS) {
        free(token->content);

        token->content = decoded;
        token->length = declen;
    } else {
        cli_errmsg("cli_pdf: error occurred parsing byte %lu of %lu\n",
                   (unsigned long)(length-stream.avail_in), (unsigned long)(token->length));
        free(decoded);
    }

    return rc;
}

static int filter_asciihexdecode(struct pdf_token *token)
{
    uint8_t *decoded;

    const uint8_t *content = (uint8_t *)token->content;
    uint32_t length = token->length;
    uint32_t i, j;
    int rc = CL_SUCCESS;

    if (!(decoded = (uint8_t *)cli_calloc(length/2 + 1, sizeof(uint8_t)))) {
        cli_errmsg("cli_pdf: cannot allocate memory for decoded output\n");
        return CL_EMEM;
    }

    for (i = 0, j = 0; i+1 < length; i++) {
        if (content[i] == ' ')
            continue;

        if (content[i] == '>')
            break;

        if (cli_hex2str_to(content+i, decoded+j, 2) == -1) {
            if (length - i < 4)
                continue;

            rc = CL_EFORMAT;
            break;
        }

        i++;
        j++;
    }

    if (rc == CL_SUCCESS) {
        free(token->content);

        cli_dbgmsg("cli_pdf: deflated %lu bytes from %lu total bytes\n",
                   (unsigned long)j, (unsigned long)(token->length));

        token->content = decoded;
        token->length = j;
    } else {
        cli_errmsg("cli_pdf: error occurred parsing byte %lu of %lu\n",
                   (unsigned long)i, (unsigned long)(token->length));
        free(decoded);
    }
    return rc;
}

/* modes: 0 = use default/DecodeParms, 1 = use document setting */
static int filter_decrypt(struct pdf_struct *pdf, struct pdf_obj *obj, struct pdf_dict *params, struct pdf_token *token, int mode)
{
    char *decrypted;
    off_t length = token->length;
    enum enc_method enc = ENC_IDENTITY;

    if (mode)
        enc = get_enc_method(pdf, obj);
    else if (params) {
        struct pdf_dict_node *node = params->nodes;

        while (node) {
            if (node->type == PDF_DICT_STRING) {
                if (!strncmp(node->key, "/Type", 6)) { /* optional field - Type */
                    /* MUST be "CryptFilterDecodeParms" */
                    cli_dbgmsg("cli_pdf: Type: %s\n", (char *)(node->value));
                } else if (!strncmp(node->key, "/Name", 6)) { /* optional field - Name */
                    /* overrides document and default encryption method */
                    cli_dbgmsg("cli_pdf: Name: %s\n", (char *)(node->value));
                    enc = parse_enc_method(pdf->CF, pdf->CF_n, (char *)(node->value), enc);
                }
            }
            node = node->next;
        }
    }

    decrypted = decrypt_any(pdf, obj->id, token->content, &length, enc);
    if (!decrypted) {
        cli_dbgmsg("cli_pdf: failed to decrypt stream\n");
        return CL_EPARSE; /* TODO: what should this value be? */
    }

    cli_dbgmsg("cli_pdf: decrypted %lld bytes from %lu total bytes\n",
               (long long int)length, (long unsigned)token->length);


    free(token->content);
    token->content = (uint8_t *)decrypted;
    token->length = (uint32_t)length; /* this may truncate unfortunately, TODO: use 64-bit values internally? */
    return CL_SUCCESS;
}
