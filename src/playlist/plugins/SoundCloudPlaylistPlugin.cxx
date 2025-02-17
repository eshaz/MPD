// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "SoundCloudPlaylistPlugin.hxx"
#include "../PlaylistPlugin.hxx"
#include "../MemorySongEnumerator.hxx"
#include "lib/yajl/Handle.hxx"
#include "lib/yajl/Callbacks.hxx"
#include "lib/yajl/ParseInputStream.hxx"
#include "config/Block.hxx"
#include "input/InputStream.hxx"
#include "tag/Builder.hxx"
#include "util/AllocatedString.hxx"
#include "util/ASCII.hxx"
#include "util/StringCompare.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <string>

#include <string.h>
#include <stdlib.h>

using std::string_view_literals::operator""sv;

static struct {
	std::string apikey;
} soundcloud_config;

static constexpr Domain soundcloud_domain("soundcloud");

static bool
soundcloud_init(const ConfigBlock &block)
{
	// APIKEY for MPD application, registered under DarkFox' account.
	soundcloud_config.apikey = block.GetBlockValue("apikey", "a25e51780f7f86af0afa91f241d091f8");
	if (soundcloud_config.apikey.empty()) {
		LogDebug(soundcloud_domain,
			 "disabling the soundcloud playlist plugin "
			 "because API key is not set");
		return false;
	}

	return true;
}

/**
 * Construct a full soundcloud resolver URL from the given fragment.
 * @param uri uri of a soundcloud page (or just the path)
 * @return Constructed URL. Must be freed with free().
 */
static AllocatedString
soundcloud_resolve(std::string_view uri) noexcept
{
	if (StringStartsWithIgnoreCase(uri, "https://"sv)) {
		return AllocatedString{uri};
	} else if (uri.starts_with("soundcloud.com"sv)) {
		return AllocatedString{"https://"sv, uri};
	}


	/* assume it's just a path on soundcloud.com */
	AllocatedString u{"https://soundcloud.com/"sv, uri};

	return AllocatedString{
		"https://api.soundcloud.com/resolve.json?url="sv,
		u, "&client_id="sv,
		soundcloud_config.apikey,
	};
}

static AllocatedString
TranslateSoundCloudUri(std::string_view uri) noexcept
{
	if (SkipPrefix(uri, "track/"sv)) {
		return AllocatedString{
			"https://api.soundcloud.com/tracks/"sv,
			uri, ".json?client_id="sv,
			soundcloud_config.apikey,
		};
	} else if (SkipPrefix(uri, "playlist/"sv)) {
		return AllocatedString{
			"https://api.soundcloud.com/playlists/"sv,
			uri, ".json?client_id="sv,
			soundcloud_config.apikey,
		};
	} else if (SkipPrefix(uri, "user/"sv)) {
		return AllocatedString{
			"https://api.soundcloud.com/users/"sv,
			uri, "/tracks.json?client_id="sv,
			soundcloud_config.apikey,
		};
	} else if (SkipPrefix(uri, "search/"sv)) {
		return AllocatedString{
			"https://api.soundcloud.com/tracks.json?q="sv,
			uri, "&client_id="sv,
			soundcloud_config.apikey,
		};
	} else if (SkipPrefix(uri, "url/"sv)) {
		/* Translate to soundcloud resolver call. libcurl will automatically
		   follow the redirect to the right resource. */
		return soundcloud_resolve(uri);
	} else
		return nullptr;
}

/* YAJL parser for track data from both /tracks/ and /playlists/ JSON */

static const char *const key_str[] = {
	"duration",
	"title",
	"stream_url",
	nullptr,
};

struct SoundCloudJsonData {
	enum class Key {
		DURATION,
		TITLE,
		STREAM_URL,
		OTHER,
	};

	Key key;
	std::string stream_url;
	long duration;
	std::string title;
	int got_url = 0; /* nesting level of last stream_url */

	std::forward_list<DetachedSong> songs;

	bool Integer(long long value) noexcept;
	bool String(std::string_view value) noexcept;
	bool StartMap() noexcept;
	bool MapKey(std::string_view value) noexcept;
	bool EndMap() noexcept;
};

inline bool
SoundCloudJsonData::Integer(long long intval) noexcept
{
	switch (key) {
	case SoundCloudJsonData::Key::DURATION:
		duration = intval;
		break;
	default:
		break;
	}

	return true;
}

inline bool
SoundCloudJsonData::String(std::string_view value) noexcept
{
	switch (key) {
	case SoundCloudJsonData::Key::TITLE:
		title = value;
		break;

	case SoundCloudJsonData::Key::STREAM_URL:
		stream_url = value;
		got_url = 1;
		break;

	default:
		break;
	}

	return true;
}

inline bool
SoundCloudJsonData::MapKey(std::string_view value) noexcept
{
	const auto *i = key_str;
	while (*i != nullptr && !StringStartsWith(*i, value))
		++i;

	key = SoundCloudJsonData::Key(i - key_str);
	return true;
}

inline bool
SoundCloudJsonData::StartMap() noexcept
{
	if (got_url > 0)
		got_url++;

	return true;
}

inline bool
SoundCloudJsonData::EndMap() noexcept
{
	if (got_url > 1) {
		got_url--;
		return true;
	}

	if (got_url == 0)
		return true;

	/* got_url == 1, track finished, make it into a song */
	got_url = 0;

	const std::string u = stream_url + "?client_id=" +
		soundcloud_config.apikey;

	TagBuilder tag;
	tag.SetDuration(SignedSongTime::FromMS(duration));
	if (!title.empty())
		tag.AddItem(TAG_NAME, title.c_str());

	songs.emplace_front(u.c_str(), tag.Commit());

	return true;
}

using Wrapper = Yajl::CallbacksWrapper<SoundCloudJsonData>;
static constexpr yajl_callbacks parse_callbacks = {
	nullptr,
	nullptr,
	Wrapper::Integer,
	nullptr,
	nullptr,
	Wrapper::String,
	Wrapper::StartMap,
	Wrapper::MapKey,
	Wrapper::EndMap,
	nullptr,
	nullptr,
};

/**
 * Read JSON data and parse it using the given YAJL parser.
 * @param url URL of the JSON data.
 * @param handle YAJL parser handle.
 */
static void
soundcloud_parse_json(const char *url, Yajl::Handle &handle,
		      Mutex &mutex)
{
	auto input_stream = InputStream::OpenReady(url, mutex);
	Yajl::ParseInputStream(handle, *input_stream);
}

/**
 * Parse a soundcloud:// URL and create a playlist.
 * @param uri A soundcloud URL. Accepted forms:
 *	soundcloud://track/<track-id>
 *	soundcloud://playlist/<playlist-id>
 *	soundcloud://url/<url or path of soundcloud page>
 */
static std::unique_ptr<SongEnumerator>
soundcloud_open_uri(const char *uri, Mutex &mutex)
{
	assert(StringEqualsCaseASCII(uri, "soundcloud://", 13));
	uri += 13;

	auto u = TranslateSoundCloudUri(uri);
	if (u == nullptr) {
		LogWarning(soundcloud_domain, "unknown soundcloud URI");
		return nullptr;
	}

	SoundCloudJsonData data;
	Yajl::Handle handle(&parse_callbacks, nullptr, &data);
	soundcloud_parse_json(u.c_str(), handle, mutex);

	data.songs.reverse();
	return std::make_unique<MemorySongEnumerator>(std::move(data.songs));
}

static const char *const soundcloud_schemes[] = {
	"soundcloud",
	nullptr
};

const PlaylistPlugin soundcloud_playlist_plugin =
	PlaylistPlugin("soundcloud", soundcloud_open_uri)
	.WithInit(soundcloud_init)
	.WithSchemes(soundcloud_schemes);
