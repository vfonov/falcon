#include "readCSV.h"

#include "rapidcsv.h"

#ifdef HAVE_ZLIB
#include "zstr.hpp"
#endif

namespace zstr
{
    struct memory_buffer : public std::streambuf
    {
        char * p_start {nullptr};
        char * p_end {nullptr};
        size_t size;

        memory_buffer(char const * first_elem, size_t size)
            : p_start(const_cast<char*>(first_elem)), p_end(p_start + size), size(size)
        {
            setg(p_start, p_start, p_end);
        }

        pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override
        {
            (void)which;
            if (dir == std::ios_base::cur) gbump(static_cast<int>(off));
            else setg(p_start, (dir == std::ios_base::beg ? p_start : p_end) + off, p_end);
            return gptr() - p_start;
        }

        pos_type seekpos(pos_type pos, std::ios_base::openmode which) override
        {
            return seekoff(pos, std::ios_base::beg, which);
        }
    };

    struct memory_stream : virtual memory_buffer, public std::istream
    {
        memory_stream(char const * first_elem, size_t size)
            : memory_buffer(first_elem, size), std::istream(static_cast<std::streambuf*>(this)) {}
    };
}


template <typename Derived>
  bool igl::readCSV(
    const std::string & csv_file, 
    Eigen::PlainObjectBase<Derived> & M,
    std::vector<std::string> &header,
    bool skip_header
    )
{
    try 
    {
#if  defined(HAVE_ZLIB)
        zstr::ifstream in_zstream(csv_file);
        std::vector<char> byte_buffer;
        
        while(!in_zstream.bad() && !in_zstream.eof())
        {
            char buf[0xffff];
            in_zstream.read(buf, sizeof(buf));
            if(!in_zstream.gcount()) break;
            byte_buffer.insert(std::end(byte_buffer), buf, buf+in_zstream.gcount());
        }
        zstr::memory_stream memstream((char*)byte_buffer.data(), byte_buffer.size());
        rapidcsv::Document doc(memstream, rapidcsv::LabelParams(skip_header?-1:0, -1));
#else 
        rapidcsv::Document doc(csv_file, rapidcsv::LabelParams(skip_header?-1:0, -1));
#endif

        M.resize(doc.GetRowCount(),doc.GetColumnCount());

        for(size_t i=0;i<doc.GetRowCount();++i)
        {
            for(size_t j=0;j<doc.GetColumnCount();++j)
            {
                M(i,j) = doc.template GetCell<typename Derived::Scalar>(j,i);
            }
        }
        header=doc.GetColumnNames(); //if skip_header==false ?
        return true;
    } catch (const std::exception & e) { 
        std::cerr << "readCSV error: " << csv_file << " " << e.what() << std::endl;
    }
    return false;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
#endif
