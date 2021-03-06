#include "Parser.h"
#include "Resolver.h"
#include "Interp.h"
#include "Kr/KrString.h"

#include <stdio.h>
#include <stdlib.h>

bool GenerateDebugCodeInfo(String code, String input, Memory_Arena *arena, String_Builder *builder);

struct Request
{
	String code;
	String input;
};

struct Code_Execution
{
	String code;
	String input;
	Memory_Arena *arena;
	String_Builder *builder;
	bool failed;
};

static Request ParseRequest(String content)
{
	Request request;
	String header = "##INPUT ";
	if (StrStartsWith(content, header))
	{
		auto pos = StrFindCharacter(content, '\n', 0);
		if (pos >= 0)
		{
			content.data[pos] = 0;
			request.input = SubStr(content, 0, pos);
			request.input = StrRemovePrefix(request.input, header.length);
			request.input = StrTrim(request.input);
			request.code  = SubStr(content, pos + 1, content.length);
			return request;
		}
		else
		{
			request.input = content;
			request.input = StrRemovePrefix(request.input, header.length);
			request.code  = "";
			return request;
		}
	}

	request.input = "";
	request.code  = content;
	return request;
}

#if PLATFORM_LINUX
#define HTTPSERVER_IMPL
#include "httpserver.h"

#include <pthread.h>

static void parser_on_error(Parser *parser) {
	Write(parser->error, "\"}");
	pthread_exit(NULL);
}

static void code_type_resolver_on_error(Code_Type_Resolver *resolver) {
	auto error = code_type_resolver_error_stream(resolver);
	Write(error, "\"}");
	pthread_exit(NULL);
}

void *ExecuteCodeThreadProc(void *param)
{
	auto exe = (Code_Execution *)param;

	InitThreadContext(0);

	exe->failed = !GenerateDebugCodeInfo(exe->code, exe->input, exe->arena, exe->builder);
	if (exe->failed)
	{
		return NULL;
	}

	return NULL;
}

void handle_request(struct http_request_s *request)
{
	auto code = http_request_body(request);

	String content;
	content.data = (uint8_t *)code.buf;
	content.length = code.len;

	Request req = ParseRequest(content);

	printf("Requested code::\n%s\nInput::%s\n\n", req.code.data, req.input.data);

	auto arena = MemoryArenaAllocate(MegaBytes(128));

	String_Builder builder;

	Code_Execution exe;
	exe.arena   = arena;
	exe.builder = &builder;
	exe.code    = req.code;
	exe.input   = req.input;
	exe.failed  = false;

	pthread_t thread;
	int result = pthread_create(&thread, NULL, ExecuteCodeThreadProc, &exe);
	if (result != 0)
	{
		MemoryArenaFree(arena);
		FreeBuilder(&builder);
		return;
	}

	pthread_join(thread, NULL);

	int length = 0;
	for (auto buk = &builder.head; buk; buk = buk->next)
		length += buk->written;

	uint8_t *body = PushArray(arena, uint8_t, length + 1);

	int written = 0;
	for (auto buk = &builder.head; buk; buk = buk->next)
	{
		memcpy(body + written, buk->data, buk->written);
		written += buk->written;
	}
	Assert(written == length);

	body[length] = 0;

	const char *content_type = exe.failed ? "text/plain" : "application/json";

	if (exe.failed)
	{
		fprintf(stdout, "Execution Error:\n");
		fprintf(stdout, "%.*s", length, body);
		fprintf(stdout, "\n");
	}

	struct http_response_s *response = http_response_init();
	http_response_status(response, 200);
	http_response_header(response, "Content-Type", content_type);
	http_response_header(response, "Access-Control-Allow-Origin", "*");
	http_response_header(response, "Access-Control-Allow-Headers", "*");
	http_response_body(response, (char *)body, length);
	http_respond(request, response);

	MemoryArenaFree(arena);
	FreeBuilder(&builder);
}

int main()
{
	InitThreadContext(0);

	parser_register_error_proc(parser_on_error);
	code_type_resolver_register_error_proc(code_type_resolver_on_error);

	struct http_server_s *server = http_server_init(8000, handle_request);
	http_server_listen(server);
	return 0;
}

#endif

#if PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <http.h>
#pragma comment(lib, "httpapi.lib")

DWORD WINAPI ExecuteCodeThreadProc(void *param)
{
	auto exe = (Code_Execution *)param;

	InitThreadContext(0);

	exe->failed = !GenerateDebugCodeInfo(exe->code, exe->input, exe->arena, exe->builder);
	if (exe->failed)
	{
		return 1;
	}

	return 0;
}

DWORD SendHttpResponse(HANDLE req_queue, PHTTP_REQUEST request, USHORT status, const String reason, const String content_type, const String content)
{
	HTTP_RESPONSE response;
	memset(&response, 0, sizeof(response));
	response.StatusCode = status;
	response.pReason = (char *)reason.data;
	response.ReasonLength = (USHORT)reason.length;

	response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = (char *)content_type.data;
	response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = (USHORT)content_type.length;

	HTTP_DATA_CHUNK data;

	if (content.length)
	{
		data.DataChunkType = HttpDataChunkFromMemory;
		data.FromMemory.pBuffer = content.data;
		data.FromMemory.BufferLength = (ULONG)content.length;

		response.EntityChunkCount = 1;
		response.pEntityChunks = &data;
	}

	DWORD bytes_sent = 0;
	auto result = HttpSendHttpResponse(req_queue, request->RequestId, 0, &response, NULL, &bytes_sent, NULL, 0, NULL, NULL);

	if (result != NO_ERROR)
	{
		printf("HttpSendHttpResponse failed with %lu \n", result);
	}

	return result;
}

void Listen(HANDLE req_queue)
{
	size_t request_buffer_length = sizeof(HTTP_REQUEST) + 4096;
	char *request_buffer = new char[request_buffer_length];

	if (request_buffer == NULL)
	{
		return;
	}

	PHTTP_REQUEST request = (PHTTP_REQUEST)request_buffer;

	HTTP_REQUEST_ID request_id;
	HTTP_SET_NULL_ID(&request_id);

	while (true)
	{
		memset(request, 0, request_buffer_length);

		DWORD bytes_read = 0;
		auto result = HttpReceiveHttpRequest(req_queue, request_id, 0, request, (ULONG)request_buffer_length, &bytes_read, NULL);

		if (result == NO_ERROR)
		{
			switch (request->Verb)
			{
			case HttpVerbPOST:
			{
				auto scratch = ThreadScratchpad();
				auto temp = BeginTemporaryMemory(scratch);

				const char *content_len_str = request->Headers.KnownHeaders[HttpHeaderContentLength].pRawValue;
				if (!content_len_str)
				{
					content_len_str = "0";
				}

				const int64_t allocation = atoi(content_len_str);
				int64_t allocated = allocation ? allocation : 512;

				String content;
				content.length = 0;
				content.data = (uint8_t *)PushSize(scratch, allocation);

				while (true)
				{
					result = HttpReceiveRequestEntityBody(req_queue, request->RequestId, 0, content.data + content.length, (ULONG)allocated, &bytes_read, NULL);
					content.length += bytes_read;
					if (result != ERROR_HANDLE_EOF)
						break;
					allocated -= bytes_read;
					if (allocated <= 0)
					{
						Assert(allocated == 0);
						PushSize(scratch, allocation);
						allocated = allocation;
					}
				}

				Request req = ParseRequest(content);
				printf("Requested code::\n%s\nInput::%s\n\n", req.code.data, req.input.data);

				String_Builder builder;
				auto arena = MemoryArenaAllocate(MegaBytes(128));

				Code_Execution exe;
				exe.arena   = arena;
				exe.builder = &builder;
				exe.code    = req.code;
				exe.input   = req.input;
				exe.failed  = false;

				HANDLE thread = CreateThread(nullptr, 0, ExecuteCodeThreadProc, &exe, 0, nullptr);
				WaitForSingleObject(thread, INFINITE);
				CloseHandle(thread);

				if (exe.failed)
				{
					fprintf(stdout, "Execution Error:\n");
					for (auto buk = &builder.head; buk; buk = buk->next)
					{
						fprintf(stdout, "%.*s", (int)buk->written, buk->data);
					}
					fprintf(stdout, "\n");
				}

				const String origin_name = "Access-Control-Allow-Origin";
				const String headers_name = "Access-Control-Allow-Headers";
				const String value = "*";

				HTTP_UNKNOWN_HEADER unknown_headers[2];
				unknown_headers[0].NameLength = (USHORT)origin_name.length;
				unknown_headers[0].RawValueLength = (USHORT)value.length;
				unknown_headers[0].pName = (char *)origin_name.data;
				unknown_headers[0].pRawValue = (char *)value.data;

				unknown_headers[1].NameLength = (USHORT)headers_name.length;
				unknown_headers[1].RawValueLength = (USHORT)value.length;
				unknown_headers[1].pName = (char *)headers_name.data;
				unknown_headers[1].pRawValue = (char *)value.data;

				const String reason = "OK";
				HTTP_RESPONSE response;
				memset(&response, 0, sizeof(response));
				response.StatusCode = 200;
				response.pReason = (char *)reason.data;
				response.ReasonLength = (USHORT)reason.length;

				String content_type = "application/json";
				response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = (char *)content_type.data;
				response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = (USHORT)content_type.length;

				response.Headers.UnknownHeaderCount = (USHORT)ArrayCount(unknown_headers);
				response.Headers.pUnknownHeaders = unknown_headers;

				int chunk_count = 0;
				for (auto buk = &builder.head; buk; buk = buk->next)
				{
					chunk_count += 1;
				}

				HTTP_DATA_CHUNK *data = PushArrayAligned(scratch, HTTP_DATA_CHUNK, chunk_count, sizeof(size_t));

				int chunk_index = 0;
				int content_len = 0;
				for (auto buk = &builder.head; buk; buk = buk->next)
				{
					data[chunk_index].DataChunkType = HttpDataChunkFromMemory;
					data[chunk_index].FromMemory.pBuffer = buk->data;
					data[chunk_index].FromMemory.BufferLength = buk->written;
					chunk_index += 1;
					content_len += buk->written;
				}

				response.EntityChunkCount = chunk_count;
				response.pEntityChunks = data;

				DWORD bytes_sent = 0;
				result = HttpSendHttpResponse(req_queue, request->RequestId, 0, &response, NULL, &bytes_sent, NULL, 0, NULL, NULL);

				if (result != NO_ERROR)
				{
					printf("HttpSendHttpResponse failed with %lu \n", result);
				}

				MemoryArenaFree(arena);
				FreeBuilder(&builder);
				EndTemporaryMemory(&temp);
			}
			break;

			default:
			{
				result = SendHttpResponse(req_queue, request, 503, "Not Implemented", "text/html", "");
			}
			break;
			}

			if (result != NO_ERROR)
			{
				break;
			}

			HTTP_SET_NULL_ID(&request_id);
		}
		else if (result == ERROR_MORE_DATA)
		{
			request_id = request->RequestId;

			char *request_buffer = new char[request_buffer_length];

			request_buffer_length = bytes_read;
			delete[] request_buffer;
			request_buffer = new char[request_buffer_length];

			if (request == NULL)
				break;
			request = (PHTTP_REQUEST)request_buffer;
		}
		else if (ERROR_CONNECTION_INVALID == result && !HTTP_IS_NULL_ID(&request_id))
		{
			HTTP_SET_NULL_ID(&request_id);
		}
		else
		{
			break;
		}
	}
}

static void parser_on_error(Parser *parser) {
	Write(parser->error, "\"}");
	ExitThread(1);
}

static void code_type_resolver_on_error(Code_Type_Resolver *resolver) {
	auto error = code_type_resolver_error_stream(resolver);
	Write(error, "\"}");
	ExitThread(1);
}

int main()
{
	InitThreadContext(MegaBytes(16));

	parser_register_error_proc(parser_on_error);
	code_type_resolver_register_error_proc(code_type_resolver_on_error);

	auto result = HttpInitialize(HTTPAPI_VERSION_1, HTTP_INITIALIZE_SERVER, NULL);

	if (result != NO_ERROR)
	{
		printf("HttpInitialize failed with % lu \n", result);
		return 1;
	}

	HANDLE req_queue = nullptr;
	result = HttpCreateRequestQueue(HTTPAPI_VERSION_1, nullptr, nullptr, 0, &req_queue);

	if (result != NO_ERROR)
	{
		printf("HttpCreateHttpHandle failed with %lu \n", result);
		CloseHandle(req_queue);
		HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
		return 1;
	}

	result = HttpAddUrl(req_queue, L"http://localhost:8000/", NULL);

	if (result != NO_ERROR)
	{
		printf("HttpAddUrl failed with %lu \n", result);
		CloseHandle(req_queue);
		HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
		return 1;
	}

	Listen(req_queue);

	CloseHandle(req_queue);
	HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
	return 0;
}

#endif
