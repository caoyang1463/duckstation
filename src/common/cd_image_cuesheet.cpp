#include "YBaseLib/Log.h"
#include "cd_image.h"
#include <libcue/libcue.h>
#include <map>
Log_SetChannel(CDImageCueSheet);

class CDImageCueSheet : public CDImage
{
public:
  CDImageCueSheet();
  ~CDImageCueSheet() override;

  bool OpenAndParse(const char* filename);

private:
  Cd* m_cd = nullptr;
  std::map<std::string, std::FILE*> m_files;
};

CDImageCueSheet::CDImageCueSheet() = default;

CDImageCueSheet::~CDImageCueSheet()
{
  std::for_each(m_files.begin(), m_files.end(), [](const auto& it) { std::fclose(it.second); });
  cd_delete(m_cd);
}

static std::string GetPathDirectory(const char* path)
{
  const char* forwardslash_ptr = std::strrchr(path, '/');
  const char* backslash_ptr = std::strrchr(path, '\\');
  const char* slash_ptr;
  if (forwardslash_ptr && backslash_ptr)
    slash_ptr = std::min(forwardslash_ptr, backslash_ptr);
  else if (backslash_ptr)
    slash_ptr = backslash_ptr;
  else if (forwardslash_ptr)
    slash_ptr = forwardslash_ptr;
  else
    return {};

  std::string str;
  str.append(path, slash_ptr - path + 1);
  return str;
}

bool CDImageCueSheet::OpenAndParse(const char* filename)
{
  std::FILE* cue_fp = std::fopen(filename, "rb");
  if (!cue_fp)
  {
    Log_ErrorPrintf("Failed to open cuesheet '%s'", filename);
    return false;
  }

  m_cd = cue_parse_file(cue_fp);
  std::fclose(cue_fp);
  if (!m_cd)
  {
    Log_ErrorPrintf("Failed to parse cuesheet '%s'", filename);
    return false;
  }

  // get the directory of the filename
  std::string basepath = GetPathDirectory(filename);
  m_filename = filename;

  u32 disc_lba = 0;

  // for each track..
  const int num_tracks = cd_get_ntrack(m_cd);
  for (int track_num = 1; track_num <= num_tracks; track_num++)
  {
    const ::Track* track = cd_get_track(m_cd, track_num);
    const std::string track_filename = track_get_filename(track);
    long track_start = track_get_start(track);
    long track_length = track_get_length(track);

    auto it = m_files.find(track_filename);
    if (it == m_files.end())
    {
      std::string track_full_filename = basepath + track_filename;
      std::FILE* track_fp = std::fopen(track_full_filename.c_str(), "rb");
      if (!track_fp)
      {
        Log_ErrorPrintf("Failed to open track filename '%s' (from '%s' and '%s')", track_full_filename.c_str(),
                        track_filename.c_str(), filename);
        return false;
      }

      it = m_files.emplace(track_filename, track_fp).first;
    }

    // TODO: FIXME
    const u32 track_sector_size = RAW_SECTOR_SIZE;
    if (track_length < 0)
    {
      // determine the length from the file
      std::fseek(it->second, 0, SEEK_END);
      long file_size = std::ftell(it->second);
      std::fseek(it->second, 0, SEEK_SET);

      file_size /= track_sector_size;
      Assert(track_start < file_size);
      track_length = file_size - track_start;
    }

    // two seconds pregap is assumed if not specified
    long pregap_frames = track_get_zero_pre(track);
    if (pregap_frames < 0)
      pregap_frames = 2 * FRAMES_PER_SECOND;

    // create the index for the pregap
    if (pregap_frames > 0)
    {
      Index pregap_index = {};
      pregap_index.start_lba_on_disc = disc_lba;
      pregap_index.start_lba_in_track = static_cast<LBA>(static_cast<unsigned long>(-pregap_frames));
      pregap_index.length = pregap_frames;
      pregap_index.track_number = track_num;
      pregap_index.index_number = 0;
      pregap_index.is_pregap = true;
      m_indices.push_back(pregap_index);

      disc_lba += pregap_index.length;
    }

    // add the track itself
    m_tracks.push_back(
      Track{static_cast<u32>(track_num), disc_lba, static_cast<u32>(m_indices.size()), static_cast<u32>(track_length)});

    // how many indices in this track?
    Index last_index;
    last_index.start_lba_on_disc = disc_lba;
    last_index.start_lba_in_track = 0;
    last_index.track_number = track_num;
    last_index.index_number = 1;
    last_index.file = it->second;
    last_index.file_sector_size = track_sector_size;
    last_index.file_offset = 0;
    last_index.is_pregap = false;

    long last_index_offset = track_start;
    for (int index_num = 1;; index_num++)
    {
      long index_offset = track_get_index(track, index_num);
      if (index_offset < 0)
        break;

      // add an index between the track indices
      if (index_offset > last_index_offset)
      {
        last_index.length = index_offset - last_index_offset;
        m_indices.push_back(last_index);

        disc_lba += last_index.length;
        last_index.start_lba_in_track += last_index.length;
        last_index.start_lba_on_disc = disc_lba;
        last_index.length = 0;
      }

      last_index.file_offset = index_offset * last_index.file_sector_size;
      last_index.index_number = static_cast<u32>(index_num);
      last_index_offset = index_offset;
    }

    // and the last index is added here
    const long track_end_index = track_start + track_length;
    DebugAssert(track_end_index >= last_index_offset);
    if (track_end_index > last_index_offset)
    {
      last_index.length = track_end_index - last_index_offset;
      m_indices.push_back(last_index);

      disc_lba += last_index.length;
    }
  }

  m_lba_count = disc_lba;
  return Seek(1, Position{0, 0, 0});
}

std::unique_ptr<CDImage> CDImage::OpenCueSheetImage(const char* filename)
{
  std::unique_ptr<CDImageCueSheet> image = std::make_unique<CDImageCueSheet>();
  if (!image->OpenAndParse(filename))
    return {};

  return image;
}