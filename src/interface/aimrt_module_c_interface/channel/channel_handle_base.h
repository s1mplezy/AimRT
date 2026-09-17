// Copyright (c) 2023, AgiBot Inc.
// All rights reserved.

#pragma once

#include "aimrt_module_c_interface/channel/channel_context_base.h"
#include "aimrt_module_c_interface/util/function_base.h"
#include "aimrt_module_c_interface/util/string.h"
#include "aimrt_module_c_interface/util/type_support_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Operate struct for subscriber release callback
 * @note
 * Signature: void(*)()
 */
typedef struct {
  void (*invoker)(void* object);
  void (*relocator)(void* from, void* to);
  void (*destroyer)(void* object);
} aimrt_function_subscriber_release_callback_ops_t;

/**
 * @brief Operate struct for subscriber callback
 * @note
 * Signature:
 * void (*)(
 *     const aimrt_channel_context_base_t* ctx_ptr,
 *     const void* msg,
 *     aimrt_function_base_t* release_callback)
 * Input 1: Const pointer to channel subscribe context
 * Input 2: Const pointer to msg
 * Input 3: Release callback, which ops type is 'aimrt_function_subscriber_release_callback_ops_t'
 */
typedef struct {
  void (*invoker)(
      void* object,
      const aimrt_channel_context_base_t* ctx_ptr,
      const void* msg,
      aimrt_function_base_t* release_callback);
  void (*relocator)(void* from, void* to);
  void (*destroyer)(void* object);
} aimrt_function_subscriber_callback_ops_t;

/**
 * @brief Status returned by explicit loaned-message operations.
 */
typedef enum {
  AIMRT_CHANNEL_LOAN_STATUS_OK = 0,
  AIMRT_CHANNEL_LOAN_STATUS_INVALID_BACKEND_COUNT = 1,
  AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_BACKEND = 2,
  AIMRT_CHANNEL_LOAN_STATUS_UNSUPPORTED_MESSAGE_TYPE = 3,
  AIMRT_CHANNEL_LOAN_STATUS_INCOMPATIBLE_BACKEND_CONFIG = 4,
  AIMRT_CHANNEL_LOAN_STATUS_RUNTIME_CANNOT_LOAN = 5,
  AIMRT_CHANNEL_LOAN_STATUS_LOAN_UNAVAILABLE = 6,
  AIMRT_CHANNEL_LOAN_STATUS_BACKEND_ERROR = 7,
  AIMRT_CHANNEL_LOAN_STATUS_INVALID_ARGUMENT = 8,
  AIMRT_CHANNEL_LOAN_STATUS_INVALID_STATE = 9,
  AIMRT_CHANNEL_LOAN_STATUS_UNREGISTERED_MESSAGE_TYPE = 10,
} aimrt_channel_loan_status_t;

/**
 * @brief Type-erased backend-owned message loan.
 * @note A non-null release operation owns msg_ptr until publish consumes it or
 *       release is invoked. owner is reserved for runtime route validation.
 */
typedef struct {
  void* msg_ptr;
  void* impl;
  aimrt_channel_loan_status_t (*release)(void* impl, void* msg_ptr);
  const void* owner;
  void* tracking_impl;
  void (*track_release)(void* tracking_impl, uint64_t borrowed_timestamp_ns);
  uint64_t borrowed_timestamp_ns;
} aimrt_channel_loaned_message_base_t;

/**
 * @brief Prepared, non-owning publisher route for one registered message type.
 * @note The runtime owns impl. The route remains valid until runtime shutdown.
 */
typedef struct {
  void* impl;
  aimrt_channel_loan_status_t (*borrow_loaned_message)(
      void* impl,
      aimrt_channel_loaned_message_base_t* output);
  aimrt_channel_loan_status_t (*publish_loaned_message)(
      void* impl,
      const aimrt_channel_context_base_t* ctx_ptr,
      aimrt_channel_loaned_message_base_t* loaned_msg);
} aimrt_channel_loaned_publisher_base_t;

/**
 * @brief Operate struct for callback-scoped loaned subscriber callback.
 * @note
 * Signature:
 * void (*)(const aimrt_channel_context_base_t* ctx_ptr, const void* msg)
 * The message pointer is valid only until invoker returns.
 */
typedef struct {
  void (*invoker)(
      void* object,
      const aimrt_channel_context_base_t* ctx_ptr,
      const void* msg);
  void (*relocator)(void* from, void* to);
  void (*destroyer)(void* object);
} aimrt_function_subscriber_loaned_callback_ops_t;

/**
 * @brief Publisher interface
 *
 */
typedef struct {
  /**
   * @brief Function to register publish type
   * @note
   * Input 1: Implement pointer to publisher handle
   * Input 2: Msg type support
   * Return: Register result
   */
  bool (*register_publish_type)(
      void* impl, const aimrt_type_support_base_t* msg_type_support);

  /**
   * @brief Function to publish msg
   * @note
   * Input 1: Implement pointer to publisher handle
   * Input 2: Msg type name
   * Input 3: Const pointer to channel publish context
   * Input 4: Const pointer to msg
   */
  void (*publish)(
      void* impl,
      aimrt_string_view_t msg_type,
      const aimrt_channel_context_base_t* ctx_ptr,
      const void* msg);

  /**
   * @brief Function to get topic for current publisher
   * @note
   * Input 1: Implement pointer to publisher handle
   * Return: Topic for current publisher
   */
  aimrt_string_view_t (*get_topic)(void* impl);

  /**
   * @brief Function to merge subscribe context to publish context
   * @note
   * This method is the same as the method of aimrt_channel_rable_mase_t
   * Input 1: Implement pointer to channel handle
   * Input 2: Channel subscribe context
   * Input 3: Channel publish context, for output
   */
  void (*merge_subscribe_context_to_publish_context)(
      void* impl,
      const aimrt_channel_context_base_t* subscribe_ctx_ptr,
      const aimrt_channel_context_base_t* publish_ctx_ptr);

  /// Implement pointer
  void* impl;

  /**
   * @brief Prepare a stable publisher route for one registered message type.
   */
  aimrt_channel_loan_status_t (*prepare_loaned_publisher)(
      void* impl,
      aimrt_string_view_t msg_type,
      aimrt_channel_loaned_publisher_base_t* output);
} aimrt_channel_publisher_base_t;

/**
 * @brief Subscriber interface
 *
 */
typedef struct {
  /**
   * @brief Function to subscribe msg
   * @note
   * Input 1: Implement pointer to subscriber handle
   * Input 2: Msg type support
   * Input 3: Msg callback, which ops type is 'aimrt_function_subscriber_callback_ops_t'
   * Return: Subscribe result
   */
  bool (*subscribe)(
      void* impl,
      const aimrt_type_support_base_t* msg_type_support,
      aimrt_function_base_t* callback);

  /**
   * @brief Function to get topic for current subscriber
   * @note
   * Input 1: Implement pointer to subscriber handle
   * Return: Topic for current subscriber
   */
  aimrt_string_view_t (*get_topic)(void* impl);

  /// Implement pointer
  void* impl;

  /**
   * @brief Subscribe using a callback-scoped backend loan view.
   */
  aimrt_channel_loan_status_t (*subscribe_loaned)(
      void* impl,
      const aimrt_type_support_base_t* msg_type_support,
      aimrt_function_base_t* callback);
} aimrt_channel_subscriber_base_t;

/**
 * @brief Channel manager interface
 *
 */
typedef struct {
  /**
   * @brief Function to get publisher for a topic
   * Input 1: Implement pointer to channel manager
   * Input 2: Topic name
   * Output: Publisher handle
   */
  const aimrt_channel_publisher_base_t* (*get_publisher)(
      void* impl, aimrt_string_view_t topic);

  /**
   * @brief Function to get subscriber for a topic
   * Input 1: Implement pointer to channel manager
   * Input 2: Topic name
   * Output: Subscriber handle
   */
  const aimrt_channel_subscriber_base_t* (*get_subscriber)(
      void* impl, aimrt_string_view_t topic);

  /**
   * @brief Function to merge subscribe context to publish context
   * @note
   * Input 1: Implement pointer to channel handle
   * Input 2: Channel subscribe context
   * Input 3: Channel publish context, for output
   */
  void (*merge_subscribe_context_to_publish_context)(
      void* impl,
      const aimrt_channel_context_base_t* subscribe_ctx_ptr,
      const aimrt_channel_context_base_t* publish_ctx_ptr);

  /// Implement pointer
  void* impl;
} aimrt_channel_handle_base_t;

#ifdef __cplusplus
}
#endif
