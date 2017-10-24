/*
 * Barnacle.c
 *
 *  Created on: Oct 18, 2017
 *      Author: stefanth
 */

#include "main.h"
#include "stm32l4xx_hal.h"
#include "Barnacle.h"

extern RNG_HandleTypeDef hrng;

__attribute__((section(".PURO"))) const BARNACLE_ISSUED_PUBLIC IssuedCerts;
__attribute__((section(".FWRO"))) const BARNACLE_IDENTITY_PRIVATE FwDeviceId;
__attribute__((section(".FWRW"))) const BARNACLE_CACHED_DATA FwCache;

char* BarnacleIssuedCertChain()
{
    if(CertStore.magic == BARNACLEMAGIC)
    {
        for(uint32_t n = 0; n < NUMELEM(IssuedCerts.certTable); n++)
        {
            if((IssuedCerts.certTable[n].start != 0) && (IssuedCerts.certTable[n].size != 0))
            {
                return (char*)&IssuedCerts.certBag[IssuedCerts.certTable[n].start];
            }
        }
    }
    return NULL;
}

bool BarnacleInitialProvision()
{
    bool result = true;
    bool generateCerts = false;

    // Check if the platform identity is already provisioned
    if(FwDeviceId.magic != BARNACLEMAGIC)
    {
        uint8_t cdi[SHA256_DIGEST_LENGTH] = {0};
        BARNACLE_IDENTITY_PRIVATE newId = {0};

        // Generate a random device Identity from the hardware RNG
        newId.magic = BARNACLEMAGIC;
        BarnacleGetRandom(cdi, sizeof(cdi));
        if(!(result = (RiotCrypt_DeriveEccKey(&newId.pubKey,
                                             &newId.privKey,
                                             cdi, sizeof(cdi),
                                             (const uint8_t *)RIOT_LABEL_IDENTITY,
                                             lblSize(RIOT_LABEL_IDENTITY)) == RIOT_SUCCESS)))
        {
            fprintf(stderr, "ERROR: RiotCrypt_DeriveEccKey failed.\r\n");
            goto Cleanup;
        }

        // Persist the identity
        if(!(result = (BarnacleFlashPages((void*)&FwDeviceId, (void*)&newId, sizeof(newId)))))
        {
            fprintf(stderr, "ERROR: BarnacleFlashPages failed.\r\n");
            goto Cleanup;
        }

        generateCerts = true;
    }

    // Check if the platform cert are provisioned
    if(generateCerts || (IssuedCerts.magic != BARNACLEMAGIC))
    {
        BARNACLE_ISSUED_PUBLIC newCertBag = {0};
        RIOT_X509_TBS_DATA x509TBSData = { { 0 },
                                           "CyReP Device", "Microsoft", "US",
                                           "170101000000Z", "370101000000Z",
                                           "CyReP Device", "Microsoft", "US" };
        DERBuilderContext derCtx = { 0 };
        uint8_t derBuffer[DER_MAX_TBS] = { 0 };
        uint8_t digest[SHA256_DIGEST_LENGTH] = { 0 };
        char PEM[DER_MAX_PEM] = { 0 };
        uint32_t length = 0;
        RIOT_ECC_SIGNATURE  tbsSig = { 0 };

        // Make sure we don't flash unwritten space in the cert bag
        newCertBag.magic = BARNACLEMAGIC;
        memset(newCertBag.certBag, 0xff, sizeof(newCertBag.certBag) - 1);

        // Generating self-signed DeviceID certificate
        DERInitContext(&derCtx, derBuffer, DER_MAX_TBS);
        if(!(result = (RiotCrypt_Kdf(digest,
                                     sizeof(digest),
                                     (uint8_t*)&FwDeviceId.pubKey, sizeof(FwDeviceId.pubKey),
                                     NULL, 0,
                                     (const uint8_t *)RIOT_LABEL_SERIAL,
                                     lblSize(RIOT_LABEL_SERIAL),
                                     sizeof(digest)) == RIOT_SUCCESS)))
        {
            fprintf(stderr, "ERROR: RiotCrypt_Kdf failed.\r\n");
            goto Cleanup;
        }
        digest[0] &= 0x7F; // Ensure that the serial number is positive
        digest[0] |= 0x01; // Ensure that the serial is not null
        memcpy(x509TBSData.SerialNum, digest, sizeof(x509TBSData.SerialNum));
        if(!(result = (X509GetDeviceCertTBS(&derCtx,
                                           &x509TBSData,
                                           (RIOT_ECC_PUBLIC*)&FwDeviceId.pubKey,
                                           (RIOT_ECC_PUBLIC*)&FwDeviceId.pubKey,
                                           1) == 0)))
        {
            fprintf(stderr, "ERROR: X509GetDeviceCertTBS failed.\r\n");
            goto Cleanup;
        }

        // Self-sign the certificate and finalize it
        if(!(result = (RiotCrypt_Sign(&tbsSig,
                                      derCtx.Buffer,
                                      derCtx.Position,
                                      (RIOT_ECC_PRIVATE*)&FwDeviceId.privKey) == RIOT_SUCCESS)))
        {
            fprintf(stderr, "ERROR: RiotCrypt_Sign failed.\r\n");
            goto Cleanup;
        }
        if(!(result = (X509MakeDeviceCert(&derCtx, &tbsSig) == 0)))
        {
            fprintf(stderr, "ERROR: X509MakeDeviceCert failed.\r\n");
            goto Cleanup;
        }

        // Produce a PEM formatted output from the DER encoded cert
        length = sizeof(PEM);
        if(!(result = (DERtoPEM(&derCtx, CERT_TYPE, PEM, &length) == 0)))
        {
            fprintf(stderr, "ERROR: DERtoPEM failed.\r\n");
            goto Cleanup;
        }

        // Write the cert out to the new certBag. We fill from the bottom.
        newCertBag.certTable[6].start = sizeof(newCertBag.certBag) - 1 - length;
        newCertBag.certTable[6].size = (uint16_t)length;
        memcpy(&newCertBag.certBag[newCertBag.certTable[6].start], PEM, newCertBag.certTable[6].size);

        // Persist the new certBag in flash
        if(!(result = (BarnacleFlashPages((void*)&IssuedCerts, (void*)&newCertBag, sizeof(newCertBag)))))
        {
            fprintf(stderr, "ERROR: BarnacleFlashPages failed.\r\n");
            goto Cleanup;
        }
    }

Cleanup:
    return result;
}

bool BarnacleVerifyAgent()
{
    bool result = true;
    uint8_t digest[SHA256_DIGEST_LENGTH];
    RIOT_ECC_SIGNATURE sig = {0};

    // Sniff the header
    if(!(result = ((AgentHdr.sign.hdr.magic == BARNACLEMAGIC) &&
                   (AgentHdr.sign.hdr.version <= BARNACLEVERSION))))
    {
        fprintf(stderr, "ERROR: Invalid agent present.\r\n");
        goto Cleanup;
    }

    // Make sure the agent code starts where we expect it to start
    if(AgentCode != &((uint8_t*)&AgentHdr)[AgentHdr.sign.hdr.size])
    {
        fprintf(stderr, "ERROR: Unexpected agent start.\r\n");
        goto Cleanup;
    }

    // Verify the agent code digest against the header
    if(!(result = (RiotCrypt_Hash(digest,
                                  sizeof(digest),
                                  AgentCode,
                                  AgentHdr.sign.agent.size) == RIOT_SUCCESS)))
    {
        fprintf(stderr, "ERROR: RiotCrypt_Hash failed.\r\n");
        goto Cleanup;
    }
    if(!(result = (memcmp(digest, AgentHdr.sign.agent.digest, sizeof(digest)) == 0)))
    {
        fprintf(stderr, "ERROR: Agent digest mismatch.\r\n");
        goto Cleanup;
    }

    // Verify the header signature if we have a signer policy
    if(!(result = (RiotCrypt_Hash(digest,
                                  sizeof(digest),
                                  (const void*)&AgentHdr.sign,
                                  sizeof(AgentHdr.sign)) == RIOT_SUCCESS)))
    {
        fprintf(stderr, "ERROR: RiotCrypt_Hash failed.\r\n");
        goto Cleanup;
    }
    BigIntToBigVal(&sig.r, AgentHdr.signature.r, sizeof(AgentHdr.signature.r));
    BigIntToBigVal(&sig.s, AgentHdr.signature.s, sizeof(AgentHdr.signature.s));
    if((!BarnacleNullCheck((void*)&IssuedCerts.codeAuthPubKey, sizeof(IssuedCerts.codeAuthPubKey))) &&
       (!(result = (RiotCrypt_VerifyDigest(digest,
                                           sizeof(digest),
                                           &sig,
                                           &IssuedCerts.codeAuthPubKey) == RIOT_SUCCESS))))
    {
        fprintf(stderr, "ERROR: RiotCrypt_Verify failed.\r\n");
        goto Cleanup;
    }

    // Is this the first time launching this agent?
    if(memcmp(digest, FwCache.agentDigest, sizeof(digest)))
    {
        RIOT_X509_TBS_DATA x509TBSData = { { 0 },
                                           "CyReP Device", "Microsoft", "US",
                                           "170101000000Z", "370101000000Z",
                                           AgentHdr.sign.agent.name, NULL, NULL };
        DERBuilderContext derCtx = { 0 };
        uint8_t derBuffer[DER_MAX_TBS] = { 0 };
        char PEM[DER_MAX_PEM] = { 0 };
        uint32_t length = 0;
        RIOT_ECC_SIGNATURE  tbsSig = { 0 };
        BARNACLE_CACHED_DATA cache = {0};

        memset(cache.certBag, 0xff, sizeof(cache.certBag) - 1);
        memcpy(cache.agentDigest, digest, sizeof(digest));

        // Derive the agent compound key
        if(!(result = (RiotCrypt_DeriveEccKey(&cache.compoundPubKey,
                                              &cache.compoundPrivKey,
                                              digest, sizeof(digest),
                                              (const uint8_t *)RIOT_LABEL_IDENTITY,
                                              lblSize(RIOT_LABEL_IDENTITY)) == RIOT_SUCCESS)))
        {
            fprintf(stderr, "ERROR: RiotCrypt_DeriveEccKey failed.\r\n");
            goto Cleanup;
        }

        DERInitContext(&derCtx, derBuffer, DER_MAX_TBS);
        if(!(result = (RiotCrypt_Kdf(digest,
                                     sizeof(digest),
                                     (uint8_t*)&cache.compoundPubKey, sizeof(cache.compoundPubKey),
                                     NULL, 0,
                                     (const uint8_t *)RIOT_LABEL_SERIAL,
                                     lblSize(RIOT_LABEL_SERIAL),
                                     sizeof(digest)) == RIOT_SUCCESS)))
        {
            fprintf(stderr, "ERROR: RiotCrypt_Kdf failed.\r\n");
            goto Cleanup;
        }
        digest[0] &= 0x7F; // Ensure that the serial number is positive
        digest[0] |= 0x01; // Ensure that the serial is not null
        memcpy(x509TBSData.SerialNum, digest, sizeof(x509TBSData.SerialNum));
        if(!(result = (X509GetAliasCertTBS(&derCtx,
                                           &x509TBSData,
                                           (RIOT_ECC_PUBLIC*)&cache.compoundPubKey,
                                           (RIOT_ECC_PUBLIC*)&FwDeviceId.pubKey,
                                           (uint8_t*)AgentHdr.sign.agent.digest,
                                           sizeof(AgentHdr.sign.agent.digest),
                                           1) == 0)))
        {
            fprintf(stderr, "ERROR: X509GetAliasCertTBS failed.\r\n");
            goto Cleanup;
        }

        // Sign the agent compound key Certificate's TBS region
        if(!(result = (RiotCrypt_Sign(&tbsSig,
                                      derCtx.Buffer,
                                      derCtx.Position,
                                      &FwDeviceId.privKey) == RIOT_SUCCESS)))
        {
            fprintf(stderr, "ERROR: RiotCrypt_Sign failed.\r\n");
            goto Cleanup;
        }

        // Generate compound key Certificate
        if(!(result = (X509MakeAliasCert(&derCtx, &tbsSig) == 0)))
        {
            fprintf(stderr, "ERROR: X509MakeAliasCert failed.\r\n");
            goto Cleanup;
        }

        // Copy compound key Certificate
        length = sizeof(PEM);
        if(!(result = (DERtoPEM(&derCtx, CERT_TYPE, PEM, &length) == 0)))
        {
            fprintf(stderr, "ERROR: DERtoPEM failed.\r\n");
            goto Cleanup;
        }

        // Write the cert out to the new certBag. We fill from the bottom.
        cache.certTable[2].start = sizeof(cache.certBag) - 1 - length;
        cache.certTable[2].size = (uint16_t)length;
        memcpy(&cache.certBag[cache.certTable[2].start], PEM, cache.certTable[2].size);

        // Persist the new certBag in flash
        if(!(result = (BarnacleFlashPages((void*)&FwCache, (void*)&cache, sizeof(cache)))))
        {
            fprintf(stderr, "ERROR: BarnacleFlashPages failed.\r\n");
            goto Cleanup;
        }
    }

    // Copy the cached identity and cert chain to the cert store
    CompoundId.magic = BARNACLEMAGIC;
    memcpy(&CompoundId.pubKey, &FwCache.compoundPubKey, sizeof(CompoundId.pubKey));
    memcpy(&CompoundId.privKey, &FwCache.compoundPrivKey, sizeof(CompoundId.privKey));
    memset(&CertStore, 0x00, sizeof(CertStore));
    CertStore.magic = BARNACLEMAGIC;

    // First copy the issued certs into the store
    int32_t srcIndex, dstIndex, cursor;
    dstIndex = 6;
    cursor = sizeof(CertStore.certBag) - 1;
    for(srcIndex = 6; srcIndex >= 0; srcIndex--)
    {
        if(IssuedCerts.certTable[srcIndex].size == 0)
        {
            break;
        }
        CertStore.certTable[dstIndex].size = IssuedCerts.certTable[srcIndex].size;
        CertStore.certTable[dstIndex].start = (cursor -= CertStore.certTable[dstIndex].size);
        memcpy(&CertStore.certBag[CertStore.certTable[dstIndex].start],
               &IssuedCerts.certBag[IssuedCerts.certTable[srcIndex].start],
               CertStore.certTable[dstIndex].size);
        dstIndex--;
    }

    // Then Copy the dynamic cert on top
    CertStore.certTable[dstIndex].size = FwCache.certTable[2].size;
    CertStore.certTable[dstIndex].start = (cursor -= CertStore.certTable[dstIndex].size);
    memcpy(&CertStore.certBag[CertStore.certTable[dstIndex].start],
           &FwCache.certBag[FwCache.certTable[2].start],
           CertStore.certTable[dstIndex].size);
    dstIndex--;
    if(!(result = (dstIndex >= 0)))
    {
        goto Cleanup;
    }

Cleanup:
    return result;
}
