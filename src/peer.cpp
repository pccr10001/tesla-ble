// https://github.com/platformio/platform-espressif32/issues/957
// specifically set when compiling with ESP-IDF
#ifdef ESP_PLATFORM
#define MBEDTLS_CONFIG_FILE "mbedtls/esp_config.h"
#endif

#include <chrono>
#include <pb.h>
#include <inttypes.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/gcm.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha1.h>

#include "signatures.pb.h"
#include "peer.h"
#include "errors.h"

namespace TeslaBLE
{
  bool Peer::isInitialized() const
  {
    if (this->private_key_context_ == nullptr)
    {
      LOG_ERROR("Private key context is null");
      return false;
    }

    if (this->ecdh_context_ == nullptr)
    {
      LOG_ERROR("ECDH context is null");
      return false;
    }

    if (this->drbg_context_ == nullptr)
    {
      LOG_ERROR("DRBG context is null");
      return false;
    }

    if (!this->isPrivateKeyInitialized())
    {
      LOG_ERROR("Private key is not initialized");
      return false;
    }

    if (!this->isValid())
    {
      LOG_ERROR("Session is not valid");
      return false;
    }

    if (!this->hasValidEpoch())
    {
      LOG_ERROR("Peer has invalid epoch");
      return false;
    }

    return true;
  }

  bool Peer::isPrivateKeyInitialized() const
  {
    return private_key_context_ && mbedtls_pk_can_do(private_key_context_.get(), MBEDTLS_PK_ECKEY);
  }

  bool Peer::hasValidEpoch() const
  {
    // make sure epoch is not all zeros
    for (int i = 0; i < 16; i++)
    {
      if (this->epoch_[i] != 0)
      {
        return true;
      }
    }
    LOG_ERROR("Epoch is empty");
    return false;
  }

  void Peer::setCounter(const uint32_t counter)
  {
    this->counter_ = counter;
  }

  void Peer::incrementCounter()
  {
    this->counter_++;
  }

  int Peer::setEpoch(const pb_byte_t *epoch)
  {
    memcpy(this->epoch_, epoch, 16);
    return 0;
  }

  const pb_byte_t *Peer::getEpoch() const
  {
    return this->epoch_;
  }

  uint32_t Peer::generateExpiresAt(int seconds) const
  {
    uint32_t expiresAt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + std::chrono::seconds(seconds)) - this->time_zero_;
    return expiresAt;
  }

  void Peer::generateNonce(pb_byte_t *nonce) const
  {
    // random 12 bytes using rand()
    for (int i = 0; i < 12; i++)
    {
      nonce[i] = rand() % 256;
    }
  }

  void Peer::setTimeZero(const uint32_t time_zero)
  {
    this->time_zero_ = time_zero;
  }

  int Peer::loadTeslaKey(const uint8_t *public_key_buffer,
                         size_t public_key_size)
  {
    mbedtls_ecp_keypair tesla_key;
    pb_byte_t shared_secret[MBEDTLS_ECP_MAX_BYTES];
    size_t shared_secret_olen;
    pb_byte_t shared_secret_sha1[20];

    LOG_DEBUG("Initializing keypair");
    mbedtls_ecp_keypair_init(&tesla_key);
    int return_code = mbedtls_ecp_group_load(&tesla_key.private_grp, MBEDTLS_ECP_DP_SECP256R1);
    if (return_code != 0)
    {
      LOG_ERROR("Group load error: -0x%04x", (unsigned int)-return_code);
      return 1;
    }

    LOG_DEBUG("Reading public key");
    return_code = mbedtls_ecp_point_read_binary(&tesla_key.private_grp, &tesla_key.private_Q,
                                                public_key_buffer, public_key_size);
    if (return_code != 0)
    {
      LOG_ERROR("Point read error: -0x%04x", (unsigned int)-return_code);
      return 1;
    }

    LOG_DEBUG("Initializing ECDH context");
    mbedtls_ecdh_init(this->ecdh_context_.get());

    LOG_DEBUG("Generating keypair");
    return_code = mbedtls_ecdh_get_params(
        this->ecdh_context_.get(), mbedtls_pk_ec(*this->private_key_context_),
        MBEDTLS_ECDH_OURS);
    if (return_code != 0)
    {
      LOG_ERROR("ECDH Get Params (private) error: -0x%04x", (unsigned int)-return_code);
      return 1;
    }

    LOG_DEBUG("Generating shared secret");
    return_code = mbedtls_ecdh_get_params(
        this->ecdh_context_.get(), &tesla_key, MBEDTLS_ECDH_THEIRS);
    if (return_code != 0)
    {
      LOG_ERROR("ECDH Get Params (tesla) error: -0x%04x", (unsigned int)-return_code);
      return 1;
    }

    // pb_byte_t temp_shared_secret[MBEDTLS_ECP_MAX_BYTES];
    // size_t temp_shared_secret_length = 0;
    return_code =
        mbedtls_ecdh_calc_secret(this->ecdh_context_.get(), &shared_secret_olen,
                                 shared_secret, sizeof(shared_secret),
                                 mbedtls_ctr_drbg_random, this->drbg_context_.get());

    if (return_code != 0)
    {
      LOG_ERROR("ECDH calc secret error: -0x%04x", (unsigned int)-return_code);
      return 1;
    }

    // Now hash the shared secret
    // printf("shared_secret_olen: %u\n", shared_secret_olen);
    return_code = mbedtls_sha1(shared_secret, shared_secret_olen, shared_secret_sha1);
    if (return_code != 0)
    {
      LOG_ERROR("SHA 1 error: -0x%04x", (unsigned int)-return_code);
      return 1;
    }

    memcpy(this->shared_secret_sha1_, shared_secret_sha1, this->SHARED_KEY_SIZE_BYTES); // we only need the first 16 bytes
    this->setIsValid(true);
    mbedtls_ecp_keypair_free(&tesla_key);
    return 0;
  }

  int Peer::updateSession(Signatures_SessionInfo *session_info)
  {
    std::lock_guard<std::mutex> guard(this->update_mutex_);
    LOG_DEBUG("Updating session..");
    if (session_info == nullptr)
    {
      LOG_ERROR("Session info is null");
      return TeslaBLE_Status_E_ERROR_INVALID_SESSION;
    }

    int status = this->setEpoch(session_info->epoch);
    if (status != 0)
    {
      LOG_ERROR("Failed to set epoch");
      return status;
    }

    this->setCounter(session_info->counter);

    uint32_t generated_at = std::time(nullptr);
    uint32_t time_zero = generated_at - session_info->clock_time;
    this->setTimeZero(time_zero);

    // load the public key
    LOG_DEBUG("Loading Tesla key");
    int return_code = this->loadTeslaKey(session_info->publicKey.bytes, session_info->publicKey.size);
    if (return_code != 0)
    {
      LOG_ERROR("Failed to load Tesla key");
      return return_code;
    }

    this->setIsValid(true);
    return 0;
  }

  void Peer::setIsValid(bool is_valid)
  {
    this->is_valid_ = is_valid;
  }

  int Peer::ConstructADBuffer(
      Signatures_SignatureType signature_type,
      const char *VIN,
      uint32_t expires_at,
      pb_byte_t *output_buffer,
      size_t *output_length,
      uint32_t flags) const
  {
    size_t index = 0;

    // Signature type
    output_buffer[index++] = Signatures_Tag_TAG_SIGNATURE_TYPE;
    output_buffer[index++] = 0x01;
    output_buffer[index++] = signature_type;

    // Domain
    output_buffer[index++] = Signatures_Tag_TAG_DOMAIN;
    output_buffer[index++] = 0x01;
    output_buffer[index++] = this->domain;

    // Personalization (VIN)
    size_t vin_length = strlen(VIN);
    output_buffer[index++] = Signatures_Tag_TAG_PERSONALIZATION;
    output_buffer[index++] = vin_length;
    memcpy(output_buffer + index, VIN, vin_length);
    index += vin_length;

    // Epoch
    output_buffer[index++] = Signatures_Tag_TAG_EPOCH;
    output_buffer[index++] = 0x10; // Assuming epoch is always 16 bytes
    memcpy(output_buffer + index, &this->epoch_, 16);
    index += 16;

    // Expires at
    output_buffer[index++] = Signatures_Tag_TAG_EXPIRES_AT;
    output_buffer[index++] = 0x04;
    output_buffer[index++] = (expires_at >> 24) & 0xFF;
    output_buffer[index++] = (expires_at >> 16) & 0xFF;
    output_buffer[index++] = (expires_at >> 8) & 0xFF;
    output_buffer[index++] = expires_at & 0xFF;

    // Counter
    output_buffer[index++] = Signatures_Tag_TAG_COUNTER;
    output_buffer[index++] = 0x04;
    output_buffer[index++] = (this->counter_ >> 24) & 0xFF;
    output_buffer[index++] = (this->counter_ >> 16) & 0xFF;
    output_buffer[index++] = (this->counter_ >> 8) & 0xFF;
    output_buffer[index++] = this->counter_ & 0xFF;

    if(flags>0){
      // Flags
      output_buffer[index++] = Signatures_Tag_TAG_FLAGS;
      output_buffer[index++] = 0x04;
      output_buffer[index++] = (flags >> 24) & 0xFF;
      output_buffer[index++] = (flags >> 16) & 0xFF;
      output_buffer[index++] = (flags >> 8) & 0xFF;
      output_buffer[index++] = flags & 0xFF;
    }

    // Terminal byte
    output_buffer[index++] = Signatures_Tag_TAG_END;

    // // ad buffer needs to be multiple of 16
    // while (index % 16 != 0)
    // {
    //   output_buffer[index++] = 0x00;
    // }

    *output_length = index;

    return 0;
  }

  int Peer::ConstructResponseADBuffer(
      Signatures_SignatureType signature_type,
      const char *VIN,
      uint32_t custom_counter,
      uint32_t custom_expires_at,
      pb_byte_t *output_buffer,
      size_t *output_length) const
  {
    size_t index = 0;

    // Signature type
    output_buffer[index++] = Signatures_Tag_TAG_SIGNATURE_TYPE;
    output_buffer[index++] = 0x01;
    output_buffer[index++] = signature_type;

    // Domain
    output_buffer[index++] = Signatures_Tag_TAG_DOMAIN;
    output_buffer[index++] = 0x01;
    output_buffer[index++] = this->domain;

    // Personalization (VIN)
    size_t vin_length = strlen(VIN);
    output_buffer[index++] = Signatures_Tag_TAG_PERSONALIZATION;
    output_buffer[index++] = vin_length;
    memcpy(output_buffer + index, VIN, vin_length);
    index += vin_length;

    // Epoch
    output_buffer[index++] = Signatures_Tag_TAG_EPOCH;
    output_buffer[index++] = 0x10; // Assuming epoch is always 16 bytes
    memcpy(output_buffer + index, &this->epoch_, 16);
    index += 16;

    // Expires at
    output_buffer[index++] = Signatures_Tag_TAG_EXPIRES_AT;
    output_buffer[index++] = 0x04;
    output_buffer[index++] = (custom_expires_at >> 24) & 0xFF;
    output_buffer[index++] = (custom_expires_at >> 16) & 0xFF;
    output_buffer[index++] = (custom_expires_at >> 8) & 0xFF;
    output_buffer[index++] = custom_expires_at & 0xFF;

    // Custom counter (from response signature data)
    output_buffer[index++] = Signatures_Tag_TAG_COUNTER;
    output_buffer[index++] = 0x04;
    output_buffer[index++] = (custom_counter >> 24) & 0xFF;
    output_buffer[index++] = (custom_counter >> 16) & 0xFF;
    output_buffer[index++] = (custom_counter >> 8) & 0xFF;
    output_buffer[index++] = custom_counter & 0xFF;

    // Terminal byte
    output_buffer[index++] = Signatures_Tag_TAG_END;

    *output_length = index;

    return 0;
  }

  int Peer::Encrypt(pb_byte_t *input_buffer, size_t input_buffer_length,
                    pb_byte_t *output_buffer, size_t output_buffer_length,
                    size_t *output_length, pb_byte_t *signature_buffer,
                    pb_byte_t *ad_buffer, size_t ad_buffer_length, pb_byte_t nonce[12]) const
  {
    if (!isPrivateKeyInitialized())
    {
      LOG_ERROR("[Encrypt] Private key is not initialized");
      return TeslaBLE_Status_E_ERROR_PRIVATE_KEY_NOT_INITIALIZED;
    }

    mbedtls_gcm_context aes_context;
    mbedtls_gcm_init(&aes_context);

    size_t shared_secret_size = this->SHARED_KEY_SIZE_BYTES;

    if (shared_secret_size != this->SHARED_KEY_SIZE_BYTES)
    {
      LOG_ERROR("[Encrypt] Shared secret SHA1 is not 16 bytes (actual size = %u)", shared_secret_size);
      return TeslaBLE_Status_E_ERROR_ENCRYPT;
    }

    // Use 128-bit key as specified in the protocol
    int return_code = mbedtls_gcm_setkey(&aes_context, MBEDTLS_CIPHER_ID_AES, this->shared_secret_sha1_, 128);
    if (return_code != 0)
    {
      LOG_ERROR("[Encrypt] GCM set key error: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_ENCRYPT;
    }

    // Generate a new nonce for each encryption
    generateNonce(nonce);
    size_t nonce_size = 12;

    return_code = mbedtls_gcm_starts(&aes_context, MBEDTLS_GCM_ENCRYPT,
                                     nonce, nonce_size);
    if (return_code != 0)
    {
      LOG_ERROR("[Encrypt] GCM start error: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_ENCRYPT;
    }

    // Hash the AD buffer to create the AAD as per the protocol
    unsigned char ad_hash[32]; // SHA256 produces a 32-byte hash
    return_code = mbedtls_sha256(ad_buffer, ad_buffer_length, ad_hash, 0);
    if (return_code != 0)
    {
      LOG_ERROR("[Encrypt] AD metadata SHA256 hash error: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_ENCRYPT;
    }
    // Use the hash as the AAD for AES-GCM
    mbedtls_gcm_update_ad(&aes_context, ad_hash, sizeof(ad_hash));

    return_code = mbedtls_gcm_update(&aes_context, input_buffer, input_buffer_length,
                                     output_buffer, output_buffer_length, output_length);
    if (return_code != 0)
    {
      LOG_ERROR("[Encrypt] Encryption error in gcm_update: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_ENCRYPT;
    }

    size_t finish_buffer_length = 0;
    pb_byte_t finish_buffer[15]; // output_size never needs to be more than 15.
    // Finalize the encryption and get the tag
    size_t tag_length = 16; // AES-GCM typically uses a 16-byte tag
    return_code = mbedtls_gcm_finish(&aes_context, finish_buffer, sizeof(finish_buffer),
                                     &finish_buffer_length, signature_buffer, tag_length);
    if (return_code != 0)
    {
      LOG_ERROR("[Encrypt] Finalization error in gcm_finish: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_ENCRYPT;
    }

    mbedtls_gcm_free(&aes_context);

    // Log encrypted data (nonce, ciphertext, and tag)
    char nonce_hex[25];
    char output_buffer_hex[output_buffer_length * 2 + 1];
    char signature_buffer_hex[tag_length * 2 + 1];

    // Convert nonce to hex
    for (int i = 0; i < 12; i++)
    {
      snprintf(nonce_hex + (i * 2), 3, "%02x", nonce[i]);
    }
    nonce_hex[24] = '\0';

    // Convert output buffer to hex
    for (size_t i = 0; i < *output_length; i++)
    {
      snprintf(output_buffer_hex + (i * 2), 3, "%02x", output_buffer[i]);
    }
    output_buffer_hex[*output_length * 2] = '\0';

    // Convert signature buffer to hex
    for (size_t i = 0; i < tag_length; i++)
    {
      snprintf(signature_buffer_hex + (i * 2), 3, "%02x", signature_buffer[i]);
    }
    signature_buffer_hex[tag_length * 2] = '\0';

    LOG_DEBUG("[Encrypt] Nonce: %s, Ciphertext: %s, Tag: %s",
              nonce_hex, output_buffer_hex, signature_buffer_hex);

    return 0;
  }

  int Peer::Decrypt(pb_byte_t *input_buffer, size_t input_buffer_length,
                    pb_byte_t *output_buffer, size_t output_buffer_length,
                    size_t *output_length, pb_byte_t *signature_buffer,
                    pb_byte_t *ad_buffer, size_t ad_buffer_length, pb_byte_t nonce[12]) const
  {
    if (!isPrivateKeyInitialized())
    {
      LOG_ERROR("[Decrypt] Private key is not initialized");
      return TeslaBLE_Status_E_ERROR_PRIVATE_KEY_NOT_INITIALIZED;
    }

    mbedtls_gcm_context aes_context;
    mbedtls_gcm_init(&aes_context);

    size_t shared_secret_size = this->SHARED_KEY_SIZE_BYTES;

    if (shared_secret_size != this->SHARED_KEY_SIZE_BYTES)
    {
      LOG_ERROR("[Decrypt] Shared secret SHA1 is not 16 bytes (actual size = %u)", shared_secret_size);
      return TeslaBLE_Status_E_ERROR_DECRYPT;
    }

    // Use 128-bit key as specified in the protocol
    int return_code = mbedtls_gcm_setkey(&aes_context, MBEDTLS_CIPHER_ID_AES, this->shared_secret_sha1_, 128);
    if (return_code != 0)
    {
      LOG_ERROR("[Decrypt] GCM set key error: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_DECRYPT;
    }

    size_t nonce_size = 12;

    return_code = mbedtls_gcm_starts(&aes_context, MBEDTLS_GCM_DECRYPT,
                                     nonce, nonce_size);
    if (return_code != 0)
    {
      LOG_ERROR("[Decrypt] GCM start error: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_DECRYPT;
    }

    // Hash the AD buffer to create the AAD as per the protocol
    unsigned char ad_hash[32]; // SHA256 produces a 32-byte hash
    return_code = mbedtls_sha256(ad_buffer, ad_buffer_length, ad_hash, 0);
    if (return_code != 0)
    {
      LOG_ERROR("[Decrypt] AD metadata SHA256 hash error: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_DECRYPT;
    }
    // Use the hash as the AAD for AES-GCM
    mbedtls_gcm_update_ad(&aes_context, ad_hash, sizeof(ad_hash));

    return_code = mbedtls_gcm_update(&aes_context, input_buffer, input_buffer_length,
                                     output_buffer, output_buffer_length, output_length);
    if (return_code != 0)
    {
      LOG_ERROR("[Decrypt] Decryption error in gcm_update: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_DECRYPT;
    }

    size_t finish_buffer_length = 0;
    pb_byte_t finish_buffer[15]; // output_size never needs to be more than 15.
    // Finalize the decryption and verify the tag
    size_t tag_length = 16; // AES-GCM typically uses a 16-byte tag
    return_code = mbedtls_gcm_finish(&aes_context, finish_buffer, sizeof(finish_buffer),
                                     &finish_buffer_length, signature_buffer, tag_length);
    if (return_code != 0)
    {
      LOG_ERROR("[Decrypt] Finalization error in gcm_finish: -0x%04x", (unsigned int)-return_code);
      return TeslaBLE_Status_E_ERROR_DECRYPT;
    }

    mbedtls_gcm_free(&aes_context);

    // Log decrypted data
    char nonce_hex[25];
    char output_buffer_hex[*output_length * 2 + 1];
    char signature_buffer_hex[tag_length * 2 + 1];

    // Convert nonce to hex
    for (int i = 0; i < 12; i++)
    {
      snprintf(nonce_hex + (i * 2), 3, "%02x", nonce[i]);
    }
    nonce_hex[24] = '\0';

    // Convert output buffer to hex
    for (size_t i = 0; i < *output_length; i++)
    {
      snprintf(output_buffer_hex + (i * 2), 3, "%02x", output_buffer[i]);
    }
    output_buffer_hex[*output_length * 2] = '\0';

    // Convert signature buffer to hex
    for (size_t i = 0; i < tag_length; i++)
    {
      snprintf(signature_buffer_hex + (i * 2), 3, "%02x", signature_buffer[i]);
    }
    signature_buffer_hex[tag_length * 2] = '\0';

    LOG_DEBUG("[Decrypt] Nonce: %s, Plaintext: %s, Tag: %s",
              nonce_hex, output_buffer_hex, signature_buffer_hex);

    return 0;
  }
} // namespace TeslaBLE
