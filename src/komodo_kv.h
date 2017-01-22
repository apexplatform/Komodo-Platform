/******************************************************************************
 * Copyright © 2014-2017 The SuperNET Developers.                             *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * SuperNET software, including this file may be copied, modified, propagated *
 * or distributed except according to the terms contained in the LICENSE file *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

#ifndef H_KOMODOKV_H
#define H_KOMODOKV_H

int32_t komodo_kvsearch(uint256 *pubkeyp,int32_t current_height,uint32_t *flagsp,int32_t *heightp,uint8_t value[IGUANA_MAXSCRIPTSIZE],uint8_t *key,int32_t keylen)
{
    struct komodo_kv *ptr; int32_t duration,retval = -1;
    *heightp = -1;
    *flagsp = 0;
    memset(pubkeyp,0,sizeof(*pubkeyp));
    portable_mutex_lock(&KOMODO_KV_mutex);
    HASH_FIND(hh,KOMODO_KV,key,keylen,ptr);
    if ( ptr != 0 )
    {
        duration = ((ptr->flags >> 2) + 1) * KOMODO_KVDURATION;
        //printf("duration.%d flags.%d current.%d ht.%d keylen.%d valuesize.%d\n",duration,ptr->flags,current_height,ptr->height,ptr->keylen,ptr->valuesize);
        if ( current_height > (ptr->height + duration) )
        {
            HASH_DELETE(hh,KOMODO_KV,ptr);
            if ( ptr->value != 0 )
                free(ptr->value);
            if ( ptr->key != 0 )
                free(ptr->key);
            free(ptr);
        }
        else
        {
            *heightp = ptr->height;
            *flagsp = ptr->flags;
            memcpy(pubkeyp,&ptr->pubkey,sizeof(*pubkeyp));
            if ( (retval= ptr->valuesize) != 0 )
                memcpy(value,ptr->value,retval);
        }
    }
    portable_mutex_unlock(&KOMODO_KV_mutex);
    return(retval);
}

int32_t komodo_kvcmp(uint8_t *refvalue,uint16_t refvaluesize,uint8_t *value,uint16_t valuesize)
{
    if ( refvalue == 0 && value == 0 )
        return(0);
    else if ( refvalue == 0 || value == 0 )
        return(-1);
    else if ( refvaluesize != valuesize )
        return(-1);
    else return(memcmp(refvalue,value,valuesize));
}

int32_t komodo_kvnumdays(uint32_t flags)
{
    int32_t numdays;
    if ( (numdays= ((flags>>2)&0x3ff)+1) > 365 )
        numdays = 365;
    return(numdays);
}

int32_t komodo_kvduration(uint32_t flags)
{
    return(komodo_kvnumdays(flags) * KOMODO_KVDURATION);
}

uint64_t komodo_kvfee(uint32_t flags,int32_t opretlen,int32_t keylen)
{
    int32_t numdays; uint64_t fee;
    numdays = komodo_kvnumdays(flags);
    if ( (fee= (numdays*(opretlen * opretlen / keylen))) < 100000 )
        fee = 100000;
    return(fee);
}

void komodo_kvupdate(uint8_t *opretbuf,int32_t opretlen,uint64_t value)
{
    static uint256 zeroes;
    uint32_t flags; bits256 pubkey,refpubkey,sig; int32_t i,transferflag,hassig,coresize,haspubkey,height,kvheight; uint16_t keylen,valuesize,newflag = 0; uint8_t *key,*valueptr,valuebuf[IGUANA_MAXSCRIPTSIZE]; struct komodo_kv *ptr; char *transferpubstr;
    iguana_rwnum(0,&opretbuf[1],sizeof(keylen),&keylen);
    iguana_rwnum(0,&opretbuf[3],sizeof(valuesize),&valuesize);
    iguana_rwnum(0,&opretbuf[5],sizeof(height),&height);
    iguana_rwnum(0,&opretbuf[9],sizeof(flags),&flags);
    key = &opretbuf[13];
    valueptr = &key[keylen];
    fee = komodo_kvfee(flags,opretlen,keylen);
    //printf("fee %.8f vs %.8f flags.%d keylen.%d valuesize.%d height.%d (%02x %02x %02x) (%02x %02x %02x)\n",(double)fee/COIN,(double)value/COIN,flags,keylen,valuesize,height,key[0],key[1],key[2],valueptr[0],valueptr[1],valueptr[2]);
    if ( value >= fee )
    {
        coresize = (int32_t)(sizeof(flags)+sizeof(height)+sizeof(keylen)+sizeof(valuesize)+keylen+valuesize+1);
        if ( opretlen == coresize || opretlen == coresize+sizeof(bits256) || opretlen == coresize+2*sizeof(bits256) )
        {
            memset(&pubkey,0,sizeof(pubkey));
            memset(&sig,0,sizeof(sig));
            if ( (haspubkey= (opretlen >= coresize+sizeof(bits256))) != 0 )
            {
                for (i=0; i<32; i++)
                    ((uint8_t *)&pubkey)[i] = opretbuf[coresize+i];
            }
            if ( (hassig= (opretlen == coresize+sizeof(bits256)*2)) != 0 )
            {
                for (i=0; i<32; i++)
                    ((uint8_t *)&sig)[i] = opretbuf[coresize+sizeof(bits256)+i];
            }
            if ( komodo_kvsearch((uint256 *)&refpubkey,height,&flags,&kvheight,valuebuf,key,keylen) >= 0 )
            {
                if ( memcmp(&zeroes,&refpubkey,sizeof(refpubkey)) != 0 )
                {
                    if ( pubkey != refpubkey || komodo_kvsigverify(opretbuf+13,coresize-13,refpubkey,sig) < 0 )
                    {
                        printf("komodo_kvsigverify error [%d]\n",coresize-13);
                        return;
                    }
                }
            }
            portable_mutex_lock(&KOMODO_KV_mutex);
            HASH_FIND(hh,KOMODO_KV,key,keylen,ptr);
            transferflag = 0;
            if ( ptr != 0 )
            {
                if ( (ptr->flags & KOMODO_KVPROTECTED) != 0 && memcmp(&zeroes,&refpubkey,sizeof(refpubkey)) != 0 )
                {
                    transferpubstr = (char *)&value[strlen((char *)"transfer:")];
                    if ( strncmp((char *)"transfer:",(char *)valueptr,strlen((char *)"transfer:")) == 0 && is_hexstr(transferpubstr,0) == 64 )
                    {
                        transferflag = 1;
                        printf("transfer.(%s) to [%s]\n",key,transferpubstr);
                        for (i=0; i<32; i++)
                            ((uint8_t *)&pubkey)[31-i] = _decode_hex(&transferpubstr[i*2]);
                    }
                }
            }
            else if ( ptr == 0 )
            {
                ptr = (struct komodo_kv *)calloc(1,sizeof(*ptr));
                ptr->pubkey = pubkey;
                ptr->key = (uint8_t *)calloc(1,keylen);
                ptr->keylen = keylen;
                memcpy(ptr->key,key,keylen);
                newflag = 1;
                HASH_ADD_KEYPTR(hh,KOMODO_KV,ptr->key,ptr->keylen,ptr);
            }
            if ( transferflag != 0 )
            {
                ptr->pubkey = pubkey;
                ptr->height = height;
            }
            else if ( newflag != 0 || (ptr->flags & KOMODO_KVPROTECTED) == 0 )
            {
                if ( ptr->value != 0 )
                    free(ptr->value), ptr->value = 0;
                if ( (ptr->valuesize= valuesize) != 0 )
                {
                    ptr->value = (uint8_t *)calloc(1,valuesize);
                    memcpy(ptr->value,valueptr,valuesize);
                }
                ptr->height = height;
                ptr->flags = flags;
            }
            portable_mutex_unlock(&KOMODO_KV_mutex);
        } else printf("insufficient fee %.8f vs %.8f flags.%d keylen.%d valuesize.%d height.%d (%02x %02x %02x) (%02x %02x %02x)\n",(double)fee/COIN,(double)value/COIN,flags,keylen,valuesize,height,key[0],key[1],key[2],valueptr[0],valueptr[1],valueptr[2]);
    } else printf("opretlen.%d mismatch keylen.%d valuesize.%d\n",opretlen,keylen,valuesize);
}

#endif