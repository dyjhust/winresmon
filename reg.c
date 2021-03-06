#include <ntifs.h>
#include <ntddk.h>
#include "resmonk.h"

/* list of registry call:
 * NtCreateKey                  +
 * NtDeleteKey                  +
 * NtDeleteValueKey             +
 * NtEnumerateKey
 * NtEnumerateValueKey
 * NtFlushKey
 * NtInitializeRegistry
 * NtNotifyChangeKey
 * NtNotifyChangeMultipleKeys
 * NtOpenKey                    +
 * NtQueryKey
 * NtQueryValueKey              +
 * NtQueryMultipleValueKey
 * NtRestoreKey
 * NtSaveKey
 * NtSaveKeyEx
 * NtSaveMergedKeys
 * NtSetValueKey                +
 * NtLoadKey
 * NtLoadKey2
 * NtLoadKeyEx
 * NtUnloadKey
 * NtUnloadKey2
 * NtUnloadKeyEx
 * NtSetInformationKey
 * NtReplaceKey
 * NtRenameKey
 * NtQueryOpenSubKeys
 * NtQueryOpenSubKeysEx
 */

typedef struct _KSERVICE_TABLE_DESCRIPTOR {
	PULONG_PTR Base;
	PULONG Count;
	ULONG Limit;
#if defined(_IA64_)
	LONG TableBaseGpOffset;
#endif
	PUCHAR Number;
} KSERVICE_TABLE_DESCRIPTOR, *PKSERVICE_TABLE_DESCRIPTOR;
extern PKSERVICE_TABLE_DESCRIPTOR KeServiceDescriptorTable;

#define ESCAPE_CONDITION KeGetCurrentIrql() != PASSIVE_LEVEL || \
				(unsigned long)PsGetCurrentProcessId() <= 4 || \
				(unsigned long)PsGetCurrentProcessId() == daemon_pid

static const int num_Close = 25;
static NTSTATUS (*stock_Close) (HANDLE Handle);
static NTSTATUS resmon_Close   (HANDLE Handle)
{
	NTSTATUS retval;
	struct htable_entry *ht_entry;
	LARGE_INTEGER time_pre, time_post;

	if (ESCAPE_CONDITION)
		return (*stock_Close)(Handle);

	time_pre = get_timestamp();
	retval = (*stock_Close)(Handle);
	time_post = get_timestamp();

	ht_entry = htable_get_entry((unsigned long)PsGetCurrentProcessId(), Handle);
	if (ht_entry != NULL) {
		struct event *event = event_buffer_start_add();
		if (event != NULL) {
			event->type = ET_REG_CLOSE;
			event->status = retval;
			event->time_pre = time_pre;
			event->time_post = time_post;
			event->reg_close.handle = Handle;
			event->path_length = MAX_PATH_SIZE - 1 < ht_entry->name_length ? MAX_PATH_SIZE - 1 : ht_entry->name_length;
			RtlCopyMemory(event->path, ht_entry->name, event->path_length * 2 + 2);
			event_buffer_finish_add(event);
		}
//		DbgPrint("NtClose(%x, %x)\n", PsGetCurrentProcessId(), Handle);
		htable_remove_entry(ht_entry);
	}

	return retval;
}

static const int num_CreateKey = 41;
static NTSTATUS (*stock_CreateKey) (PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG TitleIndex, PUNICODE_STRING Class OPTIONAL, ULONG CreateOptions, PULONG Disposition OPTIONAL);
static NTSTATUS resmon_CreateKey   (PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG TitleIndex, PUNICODE_STRING Class OPTIONAL, ULONG CreateOptions, PULONG Disposition OPTIONAL)
{
	NTSTATUS retval;
	LARGE_INTEGER time_pre, time_post;

	if (ESCAPE_CONDITION)
		return (*stock_CreateKey)(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class, CreateOptions, Disposition);

	time_pre = get_timestamp();
	retval = (*stock_CreateKey)(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class, CreateOptions, Disposition);
	time_post = get_timestamp();

	if (ObjectAttributes->RootDirectory == NULL) {
		struct event *event = event_buffer_start_add();
//		DbgPrint("CreateKey abs (%x, %S) = %x(%x)\n", ObjectAttributes->RootDirectory, ObjectAttributes->ObjectName->Buffer, *KeyHandle, retval);
		if (event != NULL) {
			event->type = ET_REG_CREATE;
			event->status = retval;
			event->time_pre = time_pre;
			event->time_post = time_post;
			event->reg_create.handle = *KeyHandle;
			event->reg_create.desired_access = DesiredAccess;
			event->reg_create.create_options = CreateOptions;
			event->reg_create.creation_disposition = (Disposition == NULL ? 0 : *Disposition);
			event->path_length = MAX_PATH_SIZE - 1 < ObjectAttributes->ObjectName->Length / 2 ?
				MAX_PATH_SIZE - 1 : ObjectAttributes->ObjectName->Length / 2;
			RtlCopyMemory(event->path, ObjectAttributes->ObjectName->Buffer, event->path_length * 2);
			event->path[event->path_length] = 0;
			while (event->path_length > 0 && event->path[event->path_length - 1] == L'\\') {
				event->path_length --;
				event->path[event->path_length] = 0;
			}
			event_buffer_finish_add(event);
		}
		if (retval == STATUS_SUCCESS) {
			struct htable_entry *htable_entry = htable_allocate_entry();
			htable_entry->pid = (unsigned long)PsGetCurrentProcessId();
			htable_entry->handle = *KeyHandle;
			htable_entry->name_length = MAX_PATH_SIZE - 1 < ObjectAttributes->ObjectName->Length / 2 ?
				MAX_PATH_SIZE - 1 : ObjectAttributes->ObjectName->Length / 2;
			RtlCopyMemory(htable_entry->name,
					ObjectAttributes->ObjectName->Buffer,
					htable_entry->name_length * 2);
			htable_entry->name[htable_entry->name_length] = 0;
			while (htable_entry->name_length > 0 && htable_entry->name[htable_entry->name_length - 1] == L'\\') {
				htable_entry->name_length --;
				htable_entry->name[htable_entry->name_length] = 0;
			}
			htable_add_entry(htable_entry);
			htable_put_entry(htable_entry);
		}
	} else {
		struct htable_entry *parent_entry = htable_get_entry(
				(unsigned long)PsGetCurrentProcessId(),
				ObjectAttributes->RootDirectory);

		if (parent_entry == NULL) {
			void *object_body;
			char object_namei[1024];
			unsigned long ret_length;
//			DbgPrint("CreateKey mis (%x, %S) = %x(%x)\n", ObjectAttributes->RootDirectory, ObjectAttributes->ObjectName->Buffer, *KeyHandle, retval);
			if (ObReferenceObjectByHandle(ObjectAttributes->RootDirectory,
						KEY_ALL_ACCESS,
						NULL,
						KernelMode,
						&object_body,
						NULL) == STATUS_SUCCESS) {
				if (ObQueryNameString(object_body,
							(POBJECT_NAME_INFORMATION)object_namei,
							sizeof(object_namei),
							&ret_length) == STATUS_SUCCESS) {
					parent_entry = htable_allocate_entry();
					parent_entry->pid = (unsigned long)PsGetCurrentProcessId();
					parent_entry->handle = ObjectAttributes->RootDirectory;
					parent_entry->name_length =
						MAX_PATH_SIZE - 1 < ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2 ?
						MAX_PATH_SIZE - 1 : ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2;
					RtlCopyMemory(parent_entry->name,
							((POBJECT_NAME_INFORMATION)object_namei)->Name.Buffer,
							parent_entry->name_length * 2);
					parent_entry->name[parent_entry->name_length] = 0;
					htable_add_entry(parent_entry);
				}
				ObDereferenceObject(object_body);
			}
		} else {
//			DbgPrint("CreateKey hit (%x, %S) = %x(%x)\n", ObjectAttributes->RootDirectory, ObjectAttributes->ObjectName->Buffer, *KeyHandle, retval);
		}

		if (parent_entry != NULL) {
			struct event *event = event_buffer_start_add();
			int length;

			length = parent_entry->name_length + 1 + ObjectAttributes->ObjectName->Length / 2;
			if (ObjectAttributes->ObjectName->Length == 0 ||
					ObjectAttributes->ObjectName->Buffer[ObjectAttributes->ObjectName->Length / 2 - 1] == L'\\')
				length --;
			if (length >= MAX_PATH_SIZE)
				length = MAX_PATH_SIZE - 1;

			if (event != NULL) {
				event->type = ET_REG_CREATE;
				event->status = retval;
				event->time_pre = time_pre;
				event->time_post = time_post;
				event->reg_create.handle = *KeyHandle;
				event->reg_create.desired_access = DesiredAccess;
				event->reg_create.create_options = CreateOptions;
				event->reg_create.creation_disposition = (Disposition == NULL ? 0 : *Disposition);
				event->path_length = length;
				RtlCopyMemory(event->path, parent_entry->name, parent_entry->name_length * 2);
				if (length > parent_entry->name_length) {
					event->path[parent_entry->name_length] = L'\\';
					if (length - parent_entry->name_length - 1 > 0)
						RtlCopyMemory(event->path + parent_entry->name_length + 1,
								ObjectAttributes->ObjectName->Buffer,
								(length - parent_entry->name_length - 1) * 2);
				}
				event->path[length] = 0;
				event_buffer_finish_add(event);
			}
			if (retval == STATUS_SUCCESS) {
				struct htable_entry *new_entry = htable_allocate_entry();
				new_entry->pid = (unsigned long)PsGetCurrentProcessId();
				new_entry->handle = *KeyHandle;
				new_entry->name_length = length;
				RtlCopyMemory(new_entry->name, parent_entry->name, parent_entry->name_length * 2);
				if (length > parent_entry->name_length) {
					new_entry->name[parent_entry->name_length] = L'\\';
					if (length - parent_entry->name_length - 1 > 0)
						RtlCopyMemory(new_entry->name + parent_entry->name_length + 1,
								ObjectAttributes->ObjectName->Buffer,
								(length - parent_entry->name_length - 1) * 2);
				}
				new_entry->name[length] = 0;
				htable_add_entry(new_entry);
				htable_put_entry(new_entry);
			}
			htable_put_entry(parent_entry);
		} else {
			if (retval == STATUS_SUCCESS) {
//				DbgPrint("Opps! it should fail because I can't name the parent.\n");
			}
		}
	}

	return retval;
}


static const int num_DeleteKey = 63;
static NTSTATUS (*stock_DeleteKey) (HANDLE KeyHandle);
static NTSTATUS resmon_DeleteKey   (HANDLE KeyHandle)
{
	NTSTATUS retval;
	struct htable_entry *hentry;
	struct event *event;
	LARGE_INTEGER time_pre, time_post;

	if (ESCAPE_CONDITION)
		return (*stock_DeleteKey)(KeyHandle);

	time_pre = get_timestamp();
	retval = (*stock_DeleteKey)(KeyHandle);
	time_post = get_timestamp();

	hentry = htable_get_entry((unsigned long)PsGetCurrentProcessId(), KeyHandle);
	if (hentry == NULL)
		return retval;

	event = event_buffer_start_add();
	if (event != NULL) {
		event->type = ET_REG_DELETE;
		event->status = retval;
		event->time_pre = time_pre;
		event->time_post = time_post;
		event->reg_delete.handle = KeyHandle;
		event->path_length = hentry->name_length;
		RtlCopyMemory(event->path, hentry->name, hentry->name_length * 2 + 2);
		event_buffer_finish_add(event);
	}
	htable_put_entry(hentry);

	return retval;
}

static const int num_DeleteValueKey = 65;
static NTSTATUS (*stock_DeleteValueKey) (HANDLE KeyHandle, PUNICODE_STRING ValueName);
static NTSTATUS resmon_DeleteValueKey   (HANDLE KeyHandle, PUNICODE_STRING ValueName)
{
	NTSTATUS retval;
	struct htable_entry *hentry;
	LARGE_INTEGER time_pre, time_post;

	if (ESCAPE_CONDITION)
		return (*stock_DeleteValueKey)(KeyHandle, ValueName);

	time_pre = get_timestamp();
	retval = (*stock_DeleteValueKey)(KeyHandle, ValueName);
	time_post = get_timestamp();

	hentry = htable_get_entry((unsigned long)PsGetCurrentProcessId(), KeyHandle);
	if (hentry == NULL) {
		void *object_body;
		char object_namei[1024];
		unsigned long ret_length;

//		DbgPrint("DeleteValueKey mis (%x, %S) = %x\n", KeyHandle, ValueName->Buffer, retval);
		if (ObReferenceObjectByHandle(KeyHandle,
					KEY_ALL_ACCESS,
					NULL,
					KernelMode,
					&object_body,
					NULL) == STATUS_SUCCESS) {
			if (ObQueryNameString(object_body,
						(POBJECT_NAME_INFORMATION)object_namei,
						sizeof(object_namei),
						&ret_length) == STATUS_SUCCESS) {
				hentry = htable_allocate_entry();
				hentry->pid = (unsigned long)PsGetCurrentProcessId();
				hentry->handle = KeyHandle;
				hentry->name_length =
					MAX_PATH_SIZE - 1 < ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2 ?
					MAX_PATH_SIZE - 1 : ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2;
				RtlCopyMemory(hentry->name,
						((POBJECT_NAME_INFORMATION)object_namei)->Name.Buffer,
						hentry->name_length * 2);
				hentry->name[hentry->name_length] = 0;
				htable_add_entry(hentry);
			}
			ObDereferenceObject(object_body);
		}
	} else {
//		DbgPrint("DeleteValueKey hit (%x, %S) = %x\n", KeyHandle, ValueName->Buffer, retval);
	}

	if (hentry != NULL) {
		struct event *event = event_buffer_start_add();

		if (event != NULL) {
			event->type = ET_REG_DELETEVALUE;
			event->status = retval;
			event->time_pre = time_pre;
			event->time_post = time_post;
			event->reg_delete_value.handle = KeyHandle;
			event->path_length = hentry->name_length;
			RtlCopyMemory(event->path, hentry->name, hentry->name_length * 2 + 2);
			RtlCopyMemory(event->reg_delete_value.name, ValueName->Buffer,
					ValueName->Length < sizeof(event->reg_delete_value.name) - 2 ?
					ValueName->Length : sizeof(event->reg_delete_value.name) - 2);
			event->reg_delete_value.name[
				ValueName->Length / 2 < sizeof(event->reg_delete_value.name) / 2 - 1 ?
				ValueName->Length / 2 : sizeof(event->reg_delete_value.name) / 2 - 1] = 0;
			event_buffer_finish_add(event);
		}
		htable_put_entry(hentry);
	} else {
		if (retval == STATUS_SUCCESS) {
//			DbgPrint("Opps! it should fail because I can't name the parent.\n");
		}
	}

	return retval;
}

static const int num_EnumerateKey = 71;
static NTSTATUS (*stock_EnumerateKey) (HANDLE KeyHandle, ULONG Index, KEY_INFORMATION_CLASS KeyInformationClass, PVOID KeyInformation, ULONG Length, PULONG ResultLength);

static const int num_EnumerateValueKey = 73;
static NTSTATUS (*stock_EnumerateValueKey) (HANDLE KeyHandle, ULONG Index, KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass, PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);

static const int num_FlushKey = 79;
static NTSTATUS (*stock_FlushKey) (HANDLE KeyHandle);

static const int num_OpenKey = 119;
static NTSTATUS (*stock_OpenKey) (PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
static NTSTATUS resmon_OpenKey   (PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes)
{
	NTSTATUS retval;
	LARGE_INTEGER time_pre, time_post;

	if (ESCAPE_CONDITION)
		return (*stock_OpenKey)(KeyHandle, DesiredAccess, ObjectAttributes);

	time_pre = get_timestamp();
	retval = (*stock_OpenKey)(KeyHandle, DesiredAccess, ObjectAttributes);
	time_post = get_timestamp();

	if (ObjectAttributes->RootDirectory == NULL) {
		struct event *event = event_buffer_start_add();
//		DbgPrint("OpenKey abs (%x, %S) = %x(%x)\n", ObjectAttributes->RootDirectory, ObjectAttributes->ObjectName->Buffer, *KeyHandle, retval);
		if (event != NULL) {
			event->type = ET_REG_OPEN;
			event->status = retval;
			event->time_pre = time_pre;
			event->time_post = time_post;
			event->reg_open.handle = *KeyHandle;
			event->reg_open.desired_access = DesiredAccess;
			event->path_length = MAX_PATH_SIZE - 1 < ObjectAttributes->ObjectName->Length / 2 ?
				MAX_PATH_SIZE - 1 : ObjectAttributes->ObjectName->Length / 2;
			RtlCopyMemory(event->path, ObjectAttributes->ObjectName->Buffer, event->path_length * 2);
			event->path[event->path_length] = 0;
			while (event->path_length > 0 && event->path[event->path_length - 1] == L'\\') {
				event->path_length --;
				event->path[event->path_length] = 0;
			}
			event_buffer_finish_add(event);
		}
		if (retval == STATUS_SUCCESS) {
			struct htable_entry *htable_entry = htable_allocate_entry();
			htable_entry->pid = (unsigned long)PsGetCurrentProcessId();
			htable_entry->handle = *KeyHandle;
			htable_entry->name_length = MAX_PATH_SIZE - 1 < ObjectAttributes->ObjectName->Length / 2 ?
				MAX_PATH_SIZE - 1 : ObjectAttributes->ObjectName->Length / 2;
			RtlCopyMemory(htable_entry->name,
					ObjectAttributes->ObjectName->Buffer,
					htable_entry->name_length * 2);
			htable_entry->name[htable_entry->name_length] = 0;
			while (htable_entry->name_length > 0 && htable_entry->name[htable_entry->name_length - 1] == L'\\') {
				htable_entry->name_length --;
				htable_entry->name[htable_entry->name_length] = 0;
			}
			htable_add_entry(htable_entry);
			htable_put_entry(htable_entry);
		}
	} else {
		struct htable_entry *parent_entry = htable_get_entry(
				(unsigned long)PsGetCurrentProcessId(),
				ObjectAttributes->RootDirectory);

		if (parent_entry == NULL) {
			void *object_body;
			char object_namei[1024];
			unsigned long ret_length;

//			DbgPrint("OpenKey mis (%x, %S) = %x(%x)\n", ObjectAttributes->RootDirectory, ObjectAttributes->ObjectName->Buffer, *KeyHandle, retval);
			if (ObReferenceObjectByHandle(ObjectAttributes->RootDirectory,
						KEY_ALL_ACCESS,
						NULL,
						KernelMode,
						&object_body,
						NULL) == STATUS_SUCCESS) {
				if (ObQueryNameString(object_body,
							(POBJECT_NAME_INFORMATION)object_namei,
							sizeof(object_namei),
							&ret_length) == STATUS_SUCCESS) {
					parent_entry = htable_allocate_entry();
					parent_entry->pid = (unsigned long)PsGetCurrentProcessId();
					parent_entry->handle = ObjectAttributes->RootDirectory;
					parent_entry->name_length =
						MAX_PATH_SIZE - 1 < ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2 ?
						MAX_PATH_SIZE - 1 : ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2;
					RtlCopyMemory(parent_entry->name,
							((POBJECT_NAME_INFORMATION)object_namei)->Name.Buffer,
							parent_entry->name_length * 2);
					parent_entry->name[parent_entry->name_length] = 0;
					htable_add_entry(parent_entry);
				}
				ObDereferenceObject(object_body);
			}
		} else {
//			DbgPrint("OpenKey hit (%x, %S) = %x(%x)\n", ObjectAttributes->RootDirectory, ObjectAttributes->ObjectName->Buffer, *KeyHandle, retval);
		}

		if (parent_entry != NULL) {
			struct event *event = event_buffer_start_add();
			int length;

			length = parent_entry->name_length + 1 + ObjectAttributes->ObjectName->Length / 2;
			if (ObjectAttributes->ObjectName->Length == 0 ||
					ObjectAttributes->ObjectName->Buffer[ObjectAttributes->ObjectName->Length / 2 - 1] == L'\\')
				length --;
			if (length >= MAX_PATH_SIZE)
				length = MAX_PATH_SIZE - 1;

			if (event != NULL) {
				event->type = ET_REG_OPEN;
				event->status = retval;
				event->time_pre = time_pre;
				event->time_post = time_post;
				event->reg_open.handle = *KeyHandle;
				event->reg_open.desired_access = DesiredAccess;
				event->path_length = length;
				RtlCopyMemory(event->path, parent_entry->name, parent_entry->name_length * 2);
				if (length > parent_entry->name_length) {
					event->path[parent_entry->name_length] = L'\\';
					if (length - parent_entry->name_length - 1 > 0)
						RtlCopyMemory(event->path + parent_entry->name_length + 1,
								ObjectAttributes->ObjectName->Buffer,
								(length - parent_entry->name_length - 1) * 2);
				}
				event->path[length] = 0;
				event_buffer_finish_add(event);
			}
			if (retval == STATUS_SUCCESS) {
				struct htable_entry *new_entry = htable_allocate_entry();
				new_entry->pid = (unsigned long)PsGetCurrentProcessId();
				new_entry->handle = *KeyHandle;
				new_entry->name_length = length;
				RtlCopyMemory(new_entry->name, parent_entry->name, parent_entry->name_length * 2);
				if (length > parent_entry->name_length) {
						new_entry->name[parent_entry->name_length] = L'\\';
					if (length - parent_entry->name_length - 1 > 0)
						RtlCopyMemory(new_entry->name + parent_entry->name_length + 1,
								ObjectAttributes->ObjectName->Buffer,
								(length - parent_entry->name_length - 1) * 2);
				}
				new_entry->name[length] = 0;
				htable_add_entry(new_entry);
				htable_put_entry(new_entry);
			}
			htable_put_entry(parent_entry);
		} else {
			if (retval == STATUS_SUCCESS) {
//				DbgPrint("Opps! it should fail because I can't name the parent.\n");
			}
		}
	}

	return retval;
}

static const int num_QueryKey = 160;
static NTSTATUS (*stock_QueryKey) (HANDLE KeyHandle, KEY_INFORMATION_CLASS KeyInformationClass, PVOID KeyInformation, ULONG Length, PULONG ResultLength);

static const int num_QueryValueKey = 177;
static NTSTATUS (*stock_QueryValueKey) (HANDLE KeyHandle, PUNICODE_STRING ValueName, KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass, PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
static NTSTATUS resmon_QueryValueKey   (HANDLE KeyHandle, PUNICODE_STRING ValueName, KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass, PVOID KeyValueInformation, ULONG Length, PULONG ResultLength)
{
	NTSTATUS retval;
	struct htable_entry *hentry;
	LARGE_INTEGER time_pre, time_post;

	if (ESCAPE_CONDITION)
		return (*stock_QueryValueKey)(KeyHandle, ValueName, KeyValueInformationClass, KeyValueInformation, Length, ResultLength);

	time_pre = get_timestamp();
	retval = (*stock_QueryValueKey)(KeyHandle, ValueName, KeyValueInformationClass, KeyValueInformation, Length, ResultLength);
	time_post = get_timestamp();

	hentry = htable_get_entry((unsigned long)PsGetCurrentProcessId(), KeyHandle);
	if (hentry == NULL) {
		void *object_body;
		char object_namei[1024];
		unsigned long ret_length;

//		DbgPrint("QueryValueKey mis (%x, %S) = %x\n", KeyHandle, ValueName->Buffer, retval);
		if (ObReferenceObjectByHandle(KeyHandle,
					KEY_ALL_ACCESS,
					NULL,
					KernelMode,
					&object_body,
					NULL) == STATUS_SUCCESS) {
			if (ObQueryNameString(object_body,
						(POBJECT_NAME_INFORMATION)object_namei,
						sizeof(object_namei),
						&ret_length) == STATUS_SUCCESS) {
				hentry = htable_allocate_entry();
				hentry->pid = (unsigned long)PsGetCurrentProcessId();
				hentry->handle = KeyHandle;
				hentry->name_length =
					MAX_PATH_SIZE - 1 < ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2 ?
					MAX_PATH_SIZE - 1 : ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2;
				RtlCopyMemory(hentry->name,
						((POBJECT_NAME_INFORMATION)object_namei)->Name.Buffer,
						hentry->name_length * 2);
				hentry->name[hentry->name_length] = 0;
				htable_add_entry(hentry);
			}
			ObDereferenceObject(object_body);
		}
	} else {
//		DbgPrint("QueryValueKey hit (%x, %S) = %x\n", KeyHandle, ValueName->Buffer, retval);
	}

	if (hentry != NULL) {
		struct event *event = event_buffer_start_add();

		if (event != NULL) {
			event->type = ET_REG_QUERYVALUE;
			event->status = retval;
			event->time_pre = time_pre;
			event->time_post = time_post;
			event->reg_rw.handle = KeyHandle;
			if (retval == STATUS_SUCCESS) {
				switch (KeyValueInformationClass) {
				case KeyValueBasicInformation:
					event->reg_rw.value_type = ((KEY_VALUE_BASIC_INFORMATION *)KeyValueInformation)->Type;
					event->reg_rw.value_length = 0;
					event->reg_rw.value[0] = '\0';
					break;
				case KeyValueFullInformation:
					event->reg_rw.value_type = ((KEY_VALUE_FULL_INFORMATION *)KeyValueInformation)->Type;
					event->reg_rw.value_length =
						MAX_IO_SIZE - 1 < ((KEY_VALUE_FULL_INFORMATION *)KeyValueInformation)->DataLength ?
						MAX_IO_SIZE - 1 : ((KEY_VALUE_FULL_INFORMATION *)KeyValueInformation)->DataLength;
					RtlCopyMemory(event->reg_rw.value,
							(char *)KeyValueInformation + ((KEY_VALUE_FULL_INFORMATION *)KeyValueInformation)->DataOffset,
							event->reg_rw.value_length);
					event->reg_rw.value[event->reg_rw.value_length] = '\0';
					break;
				case KeyValuePartialInformation:
					event->reg_rw.value_type = ((KEY_VALUE_PARTIAL_INFORMATION *)KeyValueInformation)->Type;
					event->reg_rw.value_length =
						MAX_IO_SIZE - 1 < ((KEY_VALUE_PARTIAL_INFORMATION *)KeyValueInformation)->DataLength ?
						MAX_IO_SIZE - 1 : ((KEY_VALUE_PARTIAL_INFORMATION *)KeyValueInformation)->DataLength;
					RtlCopyMemory(event->reg_rw.value,
							&((KEY_VALUE_PARTIAL_INFORMATION *)KeyValueInformation)->Data,
							event->reg_rw.value_length);
					event->reg_rw.value[event->reg_rw.value_length] = '\0';
				}
			} else {
				event->reg_rw.value_type = 0;
				event->reg_rw.value_length = 0;
				event->reg_rw.value[0] = '\0';
			}
			event->path_length = hentry->name_length;
			RtlCopyMemory(event->path, hentry->name, hentry->name_length * 2 + 2);
			RtlCopyMemory(event->reg_rw.name, ValueName->Buffer,
					ValueName->Length < sizeof(event->reg_rw.name) - 2 ?
					ValueName->Length : sizeof(event->reg_rw.name) - 2);
			event->reg_rw.name[
				ValueName->Length / 2 < sizeof(event->reg_rw.name) / 2 - 1 ?
				ValueName->Length / 2 : sizeof(event->reg_rw.name) / 2 - 1] = 0;
			event_buffer_finish_add(event);
		}
		htable_put_entry(hentry);
	} else {
		if (retval == STATUS_SUCCESS) {
//			DbgPrint("Opps! it should fail because I can't name the parent.\n");
		}
	}

	return retval;
}

static const int num_SetValueKey = 247;
static NTSTATUS (*stock_SetValueKey) (HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex OPTIONAL, ULONG Type, PVOID Data, ULONG DataSize);
static NTSTATUS resmon_SetValueKey   (HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex OPTIONAL, ULONG Type, PVOID Data, ULONG DataSize)
{
	NTSTATUS retval;
	struct htable_entry *hentry;
	LARGE_INTEGER time_pre, time_post;

	if (ESCAPE_CONDITION)
		return (*stock_SetValueKey)(KeyHandle, ValueName, TitleIndex, Type, Data, DataSize);

	time_pre = get_timestamp();
	retval = (*stock_SetValueKey)(KeyHandle, ValueName, TitleIndex, Type, Data, DataSize);
	time_post = get_timestamp();

	hentry = htable_get_entry((unsigned long)PsGetCurrentProcessId(), KeyHandle);
	if (hentry == NULL) {
		void *object_body;
		char object_namei[1024];
		unsigned long ret_length;

//		DbgPrint("SetValueKey mis (%x, %S) = %x\n", KeyHandle, ValueName->Buffer, retval);
		if (ObReferenceObjectByHandle(KeyHandle,
					KEY_ALL_ACCESS,
					NULL,
					KernelMode,
					&object_body,
					NULL) == STATUS_SUCCESS) {
			if (ObQueryNameString(object_body,
						(POBJECT_NAME_INFORMATION)object_namei,
						sizeof(object_namei),
						&ret_length) == STATUS_SUCCESS) {
				hentry = htable_allocate_entry();
				hentry->pid = (unsigned long)PsGetCurrentProcessId();
				hentry->handle = KeyHandle;
				hentry->name_length =
					MAX_PATH_SIZE - 1 < ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2 ?
					MAX_PATH_SIZE - 1 : ((POBJECT_NAME_INFORMATION)object_namei)->Name.Length / 2;
				RtlCopyMemory(hentry->name,
						((POBJECT_NAME_INFORMATION)object_namei)->Name.Buffer,
						hentry->name_length * 2);
				hentry->name[hentry->name_length] = 0;
				htable_add_entry(hentry);
			}
			ObDereferenceObject(object_body);
		}
	} else {
//		DbgPrint("SetValueKey hit (%x, %S) = %x\n", KeyHandle, ValueName->Buffer, retval);
	}

	if (hentry != NULL) {
		struct event *event = event_buffer_start_add();

		if (event != NULL) {
			event->type = ET_REG_SETVALUE;
			event->status = retval;
			event->time_pre = time_pre;
			event->time_post = time_post;
			event->reg_rw.handle = KeyHandle;
			event->reg_rw.value_type = Type;
			event->reg_rw.value_length = MAX_IO_SIZE - 1 < DataSize ? MAX_IO_SIZE - 1 : DataSize;
			try {
				RtlCopyMemory(event->reg_rw.value, Data, event->reg_rw.value_length);
			} except (EXCEPTION_EXECUTE_HANDLER) {
				RtlZeroMemory(event->reg_rw.value, event->reg_rw.value_length);
			}
			event->reg_rw.value[event->reg_rw.value_length] = '\0';
			event->path_length = hentry->name_length;
			RtlCopyMemory(event->path, hentry->name, hentry->name_length * 2 + 2);
			RtlCopyMemory(event->reg_rw.name, ValueName->Buffer,
					ValueName->Length < sizeof(event->reg_rw.name) - 2 ?
					ValueName->Length : sizeof(event->reg_rw.name) - 2);
			event->reg_rw.name[
				ValueName->Length / 2 < sizeof(event->reg_rw.name) / 2 - 1 ?
				ValueName->Length / 2 : sizeof(event->reg_rw.name) / 2 - 1] = 0;
			event_buffer_finish_add(event);
		}
		htable_put_entry(hentry);
	} else {
		if (retval == STATUS_SUCCESS) {
//			DbgPrint("Opps! it should fail because I can't name the parent.\n");
		}
	}

	return retval;
}

NTSTATUS reg_start (void)
{
	void **entries = (void **)KeServiceDescriptorTable->Base;

	_asm
	{
		CLI                    //dissable interrupt
		MOV    EAX, CR0        //move CR0 register into EAX
		AND    EAX, NOT 10000H //disable WP bit
		MOV    CR0, EAX        //write register back
	}

	stock_Close = entries[num_Close];
	entries[num_Close] = resmon_Close;
	stock_CreateKey = entries[num_CreateKey];
	entries[num_CreateKey] = resmon_CreateKey;
	stock_DeleteKey = entries[num_DeleteKey];
	entries[num_DeleteKey] = resmon_DeleteKey;
	stock_DeleteValueKey = entries[num_DeleteValueKey];
	entries[num_DeleteValueKey] = resmon_DeleteValueKey;
	stock_OpenKey = entries[num_OpenKey];
	entries[num_OpenKey] = resmon_OpenKey;
	stock_QueryValueKey = entries[num_QueryValueKey];
	entries[num_QueryValueKey] = resmon_QueryValueKey;
	stock_SetValueKey = entries[num_SetValueKey];
	entries[num_SetValueKey] = resmon_SetValueKey;

	_asm
	{
		MOV    EAX, CR0        //move CR0 register into EAX
		OR     EAX, 10000H     //enable WP bit
		MOV    CR0, EAX        //write register back
		STI                    //enable interrupt
	}

	return STATUS_SUCCESS;
}

void reg_stop (void)
{
	void **entries = (void **)KeServiceDescriptorTable->Base;

	_asm
	{
		CLI                    //dissable interrupt
		MOV    EAX, CR0        //move CR0 register into EAX
		AND    EAX, NOT 10000H //disable WP bit
		MOV    CR0, EAX        //write register back
	}

	entries[num_Close] = stock_Close;
	entries[num_CreateKey] = stock_CreateKey;
	entries[num_DeleteKey] = stock_DeleteKey;
	entries[num_DeleteValueKey] = stock_DeleteValueKey;
	entries[num_OpenKey] = stock_OpenKey;
	entries[num_QueryValueKey] = stock_QueryValueKey;
	entries[num_SetValueKey] = stock_SetValueKey;

	_asm
	{
		MOV    EAX, CR0        //move CR0 register into EAX
		OR     EAX, 10000H     //enable WP bit
		MOV    CR0, EAX        //write register back
		STI                    //enable interrupt
	}
}
